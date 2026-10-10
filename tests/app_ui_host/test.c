#include "app_ui.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static httpd_uri_t routes[8];
static size_t route_count;
static app_http_fallback_fn fallback;
static void *fallback_context;
static esp_err_t fallback_error;
static const char *response;
static size_t response_length;
static char content_type[128];
static char cache_control[32];

esp_err_t app_http_server_register(app_http_server_t *server, const char *owner,
                                    const httpd_uri_t *handlers, size_t count)
{
    (void)server;
    assert(strcmp(owner, "app_ui") == 0 && count <= 8);
    memcpy(routes, handlers, count * sizeof(*routes));
    route_count = count;
    return ESP_OK;
}

esp_err_t app_http_server_register_fallback(app_http_server_t *server, const char *owner,
                                            app_http_fallback_fn handler, void *context)
{
    (void)server;
    assert(strcmp(owner, "app_ui") == 0);
    if (fallback_error != ESP_OK) return fallback_error;
    fallback = handler;
    fallback_context = context;
    return ESP_OK;
}

esp_err_t app_http_server_unregister_owner(app_http_server_t *server, const char *owner)
{
    (void)server;
    assert(strcmp(owner, "app_ui") == 0);
    route_count = 0;
    fallback = NULL;
    return ESP_OK;
}

esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type)
{
    (void)request;
    snprintf(content_type, sizeof(content_type), "%s", type);
    return ESP_OK;
}

esp_err_t httpd_resp_set_hdr(httpd_req_t *request, const char *name, const char *value)
{
    (void)request;
    if (strcmp(name, "Cache-Control") == 0)
        snprintf(cache_control, sizeof(cache_control), "%s", value);
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status)
{
    request->status = atoi(status);
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t *request, const char *body, size_t length)
{
    if (!request->status) request->status = 200;
    response = body;
    response_length = length;
    return ESP_OK;
}

/* Simulate shared server dispatch: registered routes precede the UI fallback. */
static int get_response(const char *uri, int method)
{
    response = NULL;
    response_length = 0;
    content_type[0] = cache_control[0] = '\0';
    httpd_req_t request = {.uri = uri, .method = method};
    size_t length = strcspn(uri, "?");
    for (size_t i = 0; i < route_count; ++i) {
        if (routes[i].method == method && strlen(routes[i].uri) == length &&
            strncmp(uri, routes[i].uri, length) == 0) {
            request.user_ctx = routes[i].user_ctx;
            assert(routes[i].handler(&request) == ESP_OK);
            return request.status;
        }
    }
    if (fallback && fallback(&request, fallback_context)) return request.status;
    return 404;
}

int main(void)
{
    assert(app_ui_start(NULL) == ESP_OK && route_count == 3 && fallback);
    assert(get_response("/", HTTP_GET) == 200);
    assert(response_length == strlen(response));
    assert(strstr(response, "href=\"/network\"") && !strstr(response, "href=\"#network\""));
    const char *index = response;
    const char *pages[] = {"/network", "/network?tab=wifi", "/future/settings",
                            "/apiary", "/assets-extra"};
    for (size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); ++i) {
        assert(get_response(pages[i], HTTP_GET) == 200 && response == index);
        assert(strcmp(content_type, "text/html; charset=utf-8") == 0);
        assert(strcmp(cache_control, "no-cache") == 0);
    }
    /* Fresh requests to the route behave exactly like refresh/direct navigation. */
    assert(get_response("/network", HTTP_GET) == 200 && response == index);
    const char *missing[] = {"/api", "/api?test=1", "/api/v1/unknown", "/assets",
                              "/assets?test=1", "/assets/missing.css", "/assets/missing.js",
                              "/favicon.ico", "/robots.txt?test=1", "/other/script.js"};
    for (size_t i = 0; i < sizeof(missing) / sizeof(missing[0]); ++i)
        assert(get_response(missing[i], HTTP_GET) == 404 && response == NULL);
    for (int method = HTTP_PUT; method <= HTTP_POST; ++method)
        assert(get_response("/network", method) == 404 && response == NULL);
    assert(get_response("/network", 99) == 404 && response == NULL);
    assert(get_response("/assets/style.css", HTTP_GET) == 200);
    assert(strcmp(content_type, "text/css; charset=utf-8") == 0 && response != index);
    assert(get_response("/assets/app.js?v=1", HTTP_GET) == 200);
    assert(strcmp(content_type, "application/javascript; charset=utf-8") == 0);
    assert(strstr(response, "location.pathname") && !strstr(response, "location.hash"));
    app_http_server_unregister_owner(NULL, "app_ui");
    fallback_error = ESP_FAIL;
    assert(app_ui_start(NULL) == ESP_FAIL && route_count == 0 && !fallback);
    puts("UI HTTP tests passed: direct/refresh page fallback, API/resource/method 404, assets and registration cleanup");
    return 0;
}
