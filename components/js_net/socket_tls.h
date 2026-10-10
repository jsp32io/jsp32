#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct socket_tls socket_tls_t;

#define SOCKET_TLS_WANT_READ (-1)
#define SOCKET_TLS_WANT_WRITE (-2)
#define SOCKET_TLS_FAILED (-3)
#define SOCKET_TLS_EOF (-4)

typedef struct {
    const char *code;
    int tls_code;
    int system_error;
    uint32_t verify_flags;
} socket_tls_error_t;

/* CA strings are packed with their NUL terminators, owned by the dial request. */
socket_tls_t *socket_tls_create(int fd, const char *server_name,
                                const char *ca, size_t ca_size, socket_tls_error_t *error);
void socket_tls_free(socket_tls_t *tls);
int socket_tls_handshake(socket_tls_t *tls, socket_tls_error_t *error);
int socket_tls_read(socket_tls_t *tls, void *data, size_t size, socket_tls_error_t *error);
int socket_tls_write(socket_tls_t *tls, const void *data, size_t size, socket_tls_error_t *error);
int socket_tls_end(socket_tls_t *tls, socket_tls_error_t *error);
size_t socket_tls_available(socket_tls_t *tls);
bool socket_tls_control_pending(socket_tls_t *tls);
