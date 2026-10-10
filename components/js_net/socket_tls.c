#include "socket_tls.h"

#include <errno.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"

#if CONFIG_JS_NET_TLS_ENABLED

#include "esp_crt_bundle.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#if !defined(MBEDTLS_HAVE_TIME_DATE)
#error "Verified JS TLS requires mbedTLS certificate validity checks"
#endif

struct socket_tls {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
    mbedtls_x509_crt ca;
    int fd;
    int system_error;
    uint32_t verify_flags;
    int (*bundle_verify)(void *, mbedtls_x509_crt *, int, uint32_t *);
    void *bundle_context;
};

static int verify_certificate(void *opaque, mbedtls_x509_crt *cert, int depth, uint32_t *flags)
{
    socket_tls_t *tls = opaque;
    int result = tls->bundle_verify(tls->bundle_context, cert, depth, flags);
    /* mbedTLS discards its failed negotiation session. Retain diagnostics only;
     * the original verifier's return and remaining flags are never relaxed. */
    tls->verify_flags |= *flags;
    return result;
}

static int send_bytes(void *opaque, const unsigned char *data, size_t size)
{
    socket_tls_t *tls = opaque;
    int count = send(tls->fd, data, size, 0);
    if (count >= 0) return count;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    tls->system_error = errno;
    return MBEDTLS_ERR_NET_SEND_FAILED;
}

static int receive_bytes(void *opaque, unsigned char *data, size_t size)
{
    socket_tls_t *tls = opaque;
    int count = recv(tls->fd, data, size, 0);
    if (count >= 0) return count;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return MBEDTLS_ERR_SSL_WANT_READ;
    tls->system_error = errno;
    return MBEDTLS_ERR_NET_RECV_FAILED;
}

void socket_tls_free(socket_tls_t *tls)
{
    if (!tls) return;
    mbedtls_ssl_free(&tls->ssl);
    mbedtls_ssl_config_free(&tls->config);
    mbedtls_x509_crt_free(&tls->ca);
    free(tls);
}

socket_tls_t *socket_tls_create(int fd, const char *server_name,
                                const char *ca, size_t ca_size, socket_tls_error_t *error)
{
    socket_tls_t *tls = calloc(1, sizeof(*tls));
    if (!tls) { error->code = "RESOURCE_LIMIT"; return NULL; }
    tls->fd = fd;
    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->config);
    mbedtls_x509_crt_init(&tls->ca);
    int result = psa_crypto_init();
    if (!result) result = mbedtls_ssl_config_defaults(&tls->config, MBEDTLS_SSL_IS_CLIENT,
                                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                                     MBEDTLS_SSL_PRESET_DEFAULT);
    if (result) goto fail;
    mbedtls_ssl_conf_authmode(&tls->config, MBEDTLS_SSL_VERIFY_REQUIRED);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    mbedtls_ssl_conf_session_tickets(&tls->config, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
    result = esp_crt_bundle_attach(&tls->config);
    if (result) goto fail;
    /* ESP-IDF/mbedTLS 4 has no accessor for the bundle's installed hook. Keep
     * this version-specific access inside the transport adapter. */
    tls->bundle_verify = tls->config.MBEDTLS_PRIVATE(f_vrfy);
    tls->bundle_context = tls->config.MBEDTLS_PRIVATE(p_vrfy);
    if (!tls->bundle_verify) { result = MBEDTLS_ERR_SSL_BAD_INPUT_DATA; goto fail; }
    for (size_t offset = 0; offset < ca_size;) {
        size_t length = strlen(ca + offset) + 1;
        const char *start = ca + offset;
        while (isspace((unsigned char)*start)) ++start;
        const char *end = strstr(start, "-----END CERTIFICATE-----");
        if (strncmp(start, "-----BEGIN CERTIFICATE-----", 27) || !end) {
            result = MBEDTLS_ERR_X509_INVALID_FORMAT;
            goto fail;
        }
        end += strlen("-----END CERTIFICATE-----");
        while (isspace((unsigned char)*end)) ++end;
        if (*end) { result = MBEDTLS_ERR_X509_INVALID_FORMAT; goto fail; }
        result = mbedtls_x509_crt_parse(&tls->ca, (const unsigned char *)ca + offset, length);
        if (result) goto fail;
        offset += length;
    }
    /* Keep the bundle verifier installed. Custom roots are local to this config;
     * chains not anchored there can still be verified by the system bundle. */
    if (ca_size) mbedtls_ssl_conf_ca_chain(&tls->config, &tls->ca, NULL);
    result = mbedtls_ssl_setup(&tls->ssl, &tls->config);
    if (!result) result = mbedtls_ssl_set_hostname(&tls->ssl, server_name);
    if (result) goto fail;
    mbedtls_ssl_set_verify(&tls->ssl, verify_certificate, tls);
    mbedtls_ssl_set_bio(&tls->ssl, tls, send_bytes, receive_bytes, NULL);
    return tls;
fail:
    error->code = result == PSA_ERROR_INSUFFICIENT_MEMORY || result == MBEDTLS_ERR_X509_ALLOC_FAILED
                ? "RESOURCE_LIMIT" : "TLS_CONFIG";
    error->tls_code = result;
    socket_tls_free(tls);
    return NULL;
}

static int result_code(socket_tls_t *tls, int result, bool handshake, socket_tls_error_t *error)
{
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    /* ssl_write can finish processing a peer ticket before writing application
     * data. The notice leaves the handshake complete; resume the same write. */
    if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return SOCKET_TLS_WANT_WRITE;
#endif
    if (result == MBEDTLS_ERR_SSL_WANT_READ) return SOCKET_TLS_WANT_READ;
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) return SOCKET_TLS_WANT_WRITE;
    if (result >= 0) return result;
    error->tls_code = result;
    error->system_error = tls->system_error;
    error->verify_flags = tls->verify_flags;
    if (handshake && (error->verify_flags || result == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED ||
                      result == MBEDTLS_ERR_X509_FATAL_ERROR)) {
        error->code = error->verify_flags & MBEDTLS_X509_BADCERT_CN_MISMATCH
                    ? "TLS_HOSTNAME" : "TLS_CERTIFICATE";
    } else if (tls->system_error == ECONNRESET || tls->system_error == EPIPE) {
        error->code = "CONNECTION_RESET";
    } else error->code = handshake ? "TLS_HANDSHAKE" : "TLS_IO";
    return SOCKET_TLS_FAILED;
}

int socket_tls_handshake(socket_tls_t *tls, socket_tls_error_t *error)
{
    return result_code(tls, mbedtls_ssl_handshake(&tls->ssl), true, error);
}

int socket_tls_read(socket_tls_t *tls, void *data, size_t size, socket_tls_error_t *error)
{
    int result = mbedtls_ssl_read(&tls->ssl, data, size);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    if (result == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) return SOCKET_TLS_WANT_READ;
#endif
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return SOCKET_TLS_EOF;
    if (result == 0 || result == MBEDTLS_ERR_SSL_CONN_EOF) {
        error->code = "TLS_TRUNCATED";
        error->tls_code = result;
        return SOCKET_TLS_FAILED;
    }
    return result_code(tls, result, false, error);
}

int socket_tls_write(socket_tls_t *tls, const void *data, size_t size, socket_tls_error_t *error)
{
    /* Keep the retry buffer and length identical across WANT_* results. */
    if (size > MBEDTLS_SSL_OUT_CONTENT_LEN) size = MBEDTLS_SSL_OUT_CONTENT_LEN;
    int maximum = mbedtls_ssl_get_max_out_record_payload(&tls->ssl);
    if (maximum > 0 && size > (size_t)maximum) size = maximum;
    return result_code(tls, mbedtls_ssl_write(&tls->ssl, data, size), false, error);
}

int socket_tls_end(socket_tls_t *tls, socket_tls_error_t *error)
{
    return result_code(tls, mbedtls_ssl_close_notify(&tls->ssl), false, error);
}

size_t socket_tls_available(socket_tls_t *tls)
{
    return mbedtls_ssl_check_pending(&tls->ssl);
}

bool socket_tls_control_pending(socket_tls_t *tls)
{
    return !mbedtls_ssl_is_handshake_over(&tls->ssl);
}

#else
socket_tls_t *socket_tls_create(int fd, const char *name, const char *ca, size_t size,
                                socket_tls_error_t *error)
{ error->code = "UNSUPPORTED"; return NULL; }
void socket_tls_free(socket_tls_t *tls) {}
int socket_tls_handshake(socket_tls_t *tls, socket_tls_error_t *error) { return SOCKET_TLS_FAILED; }
int socket_tls_read(socket_tls_t *tls, void *data, size_t size, socket_tls_error_t *error) { return SOCKET_TLS_FAILED; }
int socket_tls_write(socket_tls_t *tls, const void *data, size_t size, socket_tls_error_t *error) { return SOCKET_TLS_FAILED; }
int socket_tls_end(socket_tls_t *tls, socket_tls_error_t *error) { return SOCKET_TLS_FAILED; }
size_t socket_tls_available(socket_tls_t *tls) { return 0; }
bool socket_tls_control_pending(socket_tls_t *tls) { return false; }
#endif
