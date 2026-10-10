#include "app_ui.h"
#include <string.h>

extern const char index_start[] asm("_binary_index_html_start");
extern const char style_start[] asm("_binary_style_css_start");
extern const char script_start[] asm("_binary_app_js_start");

typedef struct { const char *type; const char *content; } ui_asset_t;
static const ui_asset_t index_asset = {"text/html; charset=utf-8", index_start};
static const ui_asset_t style_asset = {"text/css; charset=utf-8", style_start};
static const ui_asset_t script_asset = {"application/javascript; charset=utf-8", script_start};

static esp_err_t send_asset(httpd_req_t *request, const ui_asset_t *asset)
{
    httpd_resp_set_type(request, asset->type);
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(request, "Content-Security-Policy",
                       "default-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
    return httpd_resp_send(request, asset->content, strlen(asset->content));
}

static esp_err_t asset_handler(httpd_req_t *request)
{
    return send_asset(request, request->user_ctx);
}

static bool path_in_directory(const char *path, size_t length, const char *directory)
{
    size_t prefix_length = strlen(directory);
    return length >= prefix_length && strncmp(path, directory, prefix_length) == 0 &&
           (length == prefix_length || path[prefix_length] == '/');
}

static bool page_fallback(httpd_req_t *request, void *context)
{
    (void)context;
    if (request->method != HTTP_GET) return false;
    size_t length = strcspn(request->uri, "?");
    if (path_in_directory(request->uri, length, "/api") ||
        path_in_directory(request->uri, length, "/assets")) return false;
    /* File requests such as favicon.ico must retain a real 404. */
    size_t last_segment = 0;
    for (size_t i = 0; i < length; ++i) {
        if (request->uri[i] == '/') last_segment = i + 1;
    }
    if (memchr(request->uri + last_segment, '.', length - last_segment)) return false;
    httpd_resp_set_status(request, "200 OK");
    send_asset(request, &index_asset);
    return true;
}

esp_err_t app_ui_start(app_http_server_t *server)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&index_asset},
        {.uri = "/assets/style.css", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&style_asset},
        {.uri = "/assets/app.js", .method = HTTP_GET, .handler = asset_handler, .user_ctx = (void *)&script_asset},
    };
    esp_err_t err = app_http_server_register(server, "app_ui", handlers, sizeof(handlers) / sizeof(handlers[0]));
    if (err != ESP_OK) return err;
    err = app_http_server_register_fallback(server, "app_ui", page_fallback, NULL);
    if (err != ESP_OK) app_http_server_unregister_owner(server, "app_ui");
    return err;
}
