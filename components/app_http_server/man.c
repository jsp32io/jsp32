#include "app_http_server_internal.h"

#include <string.h>
#include "man_pages.h"

#define MAN_ROOT "/api/v1/man"

static esp_err_t man_handler(httpd_req_t *request)
{
    /* Exact table lookup: request input is never used as a filesystem path. */
    const char *topic = request->uri + strlen(MAN_ROOT);
    size_t length = strcspn(topic, "?");
    if (length > 0 && topic[0] == '/') {
        ++topic;
        --length;
    }
    if (length == 0) {
        topic = "index";
        length = strlen(topic);
    }
    for (size_t i = 0; i < sizeof(man_pages) / sizeof(man_pages[0]); ++i) {
        const man_page_t *page = &man_pages[i];
        size_t topic_length = strlen(page->topic);
        bool exact = topic_length == length;
        bool directory = topic_length == length + 6 &&
                         strcmp(page->topic + length, ".index") == 0;
        if ((!exact && !directory) || memcmp(page->topic, topic, length) != 0) {
            continue;
        }
        httpd_resp_set_type(request, "text/markdown; charset=utf-8");
        httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
        return httpd_resp_send(request, (const char *)page->body, page->size);
    }
    return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Manual topic not found");
}

esp_err_t man_register(app_http_server_t *server)
{
    static const httpd_uri_t handlers[] = {
        {.uri = MAN_ROOT, .method = HTTP_GET, .handler = man_handler},
        {.uri = MAN_ROOT "/*", .method = HTTP_GET, .handler = man_handler},
    };
    return app_http_server_register(server, "man", handlers,
                                    sizeof(handlers) / sizeof(handlers[0]));
}
