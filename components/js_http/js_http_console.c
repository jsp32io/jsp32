#include "js_http_internal.h"

#include <inttypes.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static void console_events_task(void *arg)
{
    httpd_req_t *request = arg;
    js_http_t *http = request->user_ctx;
    js_console_subscription_t *subscription = NULL;
    esp_err_t err = js_runtime_console_subscribe(http->runtime, &subscription);
    if (err != ESP_OK) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        httpd_resp_sendstr(request, "JavaScript runtime unavailable");
        goto complete;
    }

    httpd_resp_set_type(request, "text/event-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(request, "Connection", "keep-alive");
    httpd_resp_set_hdr(request, "X-Accel-Buffering", "no");
    err = httpd_resp_send_chunk(request, ": connected\n\n", 13);

    char line[JS_RUNTIME_CONSOLE_LINE_MAX];
    char event[JS_RUNTIME_CONSOLE_LINE_MAX + 32];
    while (err == ESP_OK && !js_http_is_stopping(http)) {
        uint32_t dropped = 0;
        esp_err_t receive = js_runtime_console_receive(
            subscription, line, sizeof(line), &dropped,
            pdMS_TO_TICKS(CONFIG_JS_HTTP_SSE_HEARTBEAT_MS));
        if (receive == ESP_ERR_NOT_FOUND) {
            int length = snprintf(event, sizeof(event),
                                  "event: dropped\ndata: %" PRIu32 "\n\n",
                                  dropped);
            if (length > 0 && (size_t)length < sizeof(event)) {
                err = httpd_resp_send_chunk(request, event, length);
            }
            continue;
        }
        if (receive == ESP_ERR_TIMEOUT) {
            err = httpd_resp_send_chunk(request, ": ping\n\n", 8);
            continue;
        }
        if (receive != ESP_OK) {
            break;
        }
        if (dropped > 0) {
            int length = snprintf(event, sizeof(event),
                                  "event: dropped\ndata: %" PRIu32 "\n\n",
                                  dropped);
            if (length > 0 && (size_t)length < sizeof(event)) {
                err = httpd_resp_send_chunk(request, event, length);
            }
        }
        if (err != ESP_OK) {
            break;
        }
        int length = snprintf(event, sizeof(event),
                              "event: console\ndata: %s\n\n", line);
        if (length > 0 && (size_t)length < sizeof(event)) {
            err = httpd_resp_send_chunk(request, event, length);
        }
    }

    js_runtime_console_unsubscribe(subscription);
    if (err == ESP_OK) {
        httpd_resp_send_chunk(request, NULL, 0);
    }

complete:
    httpd_req_async_handler_complete(request);
    js_http_console_release(http);
    vTaskDelete(NULL);
}

static esp_err_t console_events_handler(httpd_req_t *request)
{
    js_http_t *http = request->user_ctx;
    if (!js_http_console_try_acquire(http)) {
        httpd_resp_set_status(request, "503 Service Unavailable");
        return httpd_resp_sendstr(request, "Too many console clients");
    }

    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        js_http_console_release(http);
        return err;
    }
    if (xTaskCreate(console_events_task, "js_console_sse",
                    CONFIG_JS_HTTP_SSE_TASK_STACK_SIZE, async_request,
                    CONFIG_JS_HTTP_SSE_TASK_PRIORITY, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_request);
        js_http_console_release(http);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t js_http_console_register(js_http_t *http)
{
    const httpd_uri_t route = {
        .uri = "/api/v1/quickjs/console/events",
        .method = HTTP_GET,
        .handler = console_events_handler,
        .user_ctx = http,
    };
    return app_http_server_register(http->http_server, JS_HTTP_OWNER,
                                    &route, 1);
}
