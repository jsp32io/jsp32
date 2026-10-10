#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_http_server.h"

static const httpd_uri_t *routes;
static size_t route_count;
static const char *body, *content_type;
static size_t body_size;
static int status;
esp_err_t app_http_server_register(app_http_server_t *server, const char *owner,
                                   const httpd_uri_t *handlers, size_t count)
{
    (void)server;
    assert(strcmp(owner, "man") == 0);
    routes = handlers; route_count = count;
    return 0;
}
esp_err_t httpd_resp_set_type(httpd_req_t *req, const char *type)
{ (void)req; content_type = type; return 0; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *req, const char *key, const char *value)
{ (void)req; assert(strcmp(key, "Cache-Control") == 0 && strcmp(value, "no-cache") == 0); return 0; }
esp_err_t httpd_resp_send(httpd_req_t *req, const char *data, size_t size)
{ (void)req; body = data; body_size = size; return 0; }
esp_err_t httpd_resp_send_err(httpd_req_t *req, int code, const char *message)
{ (void)req; (void)message; status = code; return 0; }
extern esp_err_t man_register(app_http_server_t *server);

static void request(const char *uri)
{
    status = 200; content_type = NULL; body = NULL; body_size = 0;
    httpd_req_t req = {.uri = uri};
    assert(routes[1].handler(&req) == 0);
}

int main(int argc, char **argv)
{
    assert(man_register(NULL) == 0 && route_count == 2);
    assert(strcmp(routes[0].uri, "/api/v1/man") == 0);
    assert(strcmp(routes[1].uri, "/api/v1/man/*") == 0);
    assert(routes[0].method == HTTP_GET && routes[1].method == HTTP_GET);
    for (int i = 1; i < argc; i += 2) {
        request(argv[i]);
        assert(status == 200 && strcmp(content_type, "text/markdown; charset=utf-8") == 0);
        FILE *file = fopen(argv[i+1], "rb"); assert(file);
        fseek(file, 0, SEEK_END); long length = ftell(file); rewind(file);
        assert(length >= 0 && (size_t)length == body_size);
        char *expected = malloc(body_size + 1); assert(expected);
        assert(fread(expected, 1, body_size, file) == body_size);
        assert(memcmp(expected, body, body_size) == 0);
        free(expected); fclose(file);
    }
    const char *invalid[] = {"js.missing", "../js/gpio", "js..gpio", "js.gpio/", "js/gpio", "js.%67pio", "js.gpio.md", "js.gpio.extra"};
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i) {
        char uri[128]; snprintf(uri, sizeof(uri), "/api/v1/man/%s", invalid[i]);
        request(uri); assert(status == 404 && body == NULL);
    }
    puts("Manual HTTP host tests passed: exact Markdown bytes, directory index, query, 404, traversal.");
}
