#include "js_http.h"

#include <stdlib.h>

#include "freertos/task.h"
#include "js_http_internal.h"
#include "sdkconfig.h"

esp_err_t js_http_start(const js_http_config_t *config,
                        js_http_t **out_http)
{
    if (!config || !config->http_server || !config->runtime || !out_http) {
        return ESP_ERR_INVALID_ARG;
    }

    js_http_t *http = calloc(1, sizeof(*http));
    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    http->http_server = config->http_server;
    http->runtime = config->runtime;
    http->state_lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    http->max_sse_clients = CONFIG_JS_HTTP_MAX_SSE_CLIENTS;
#if CONFIG_JS_HTTP_EVAL_ENABLED
    http->max_eval_requests = CONFIG_JS_HTTP_MAX_PENDING_EVALS;
#endif

    esp_err_t err = js_http_console_register(http);
    if (err != ESP_OK) {
        free(http);
        return err;
    }
    err = js_http_eval_register(http);
    if (err != ESP_OK) {
        app_http_server_unregister_owner(http->http_server, JS_HTTP_OWNER);
        free(http);
        return err;
    }
    *out_http = http;
    return ESP_OK;
}

esp_err_t js_http_stop(js_http_t *http, TickType_t wait_ticks)
{
    if (!http) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&http->state_lock);
    http->stopping = true;
    portEXIT_CRITICAL(&http->state_lock);

    esp_err_t err = app_http_server_unregister_owner(http->http_server,
                                                      JS_HTTP_OWNER);
    if (err != ESP_OK) {
        return err;
    }

    TickType_t started = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&http->state_lock);
        uint32_t active = http->active_sse_clients +
                          http->active_eval_requests;
        portEXIT_CRITICAL(&http->state_lock);
        if (active == 0) {
            free(http);
            return ESP_OK;
        }
        if (wait_ticks == 0 ||
            xTaskGetTickCount() - started >= wait_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

bool js_http_console_try_acquire(js_http_t *http)
{
    if (!app_http_server_worker_try_acquire(http->http_server)) {
        return false;
    }
    bool acquired = false;
    portENTER_CRITICAL(&http->state_lock);
    if (!http->stopping &&
        http->active_sse_clients < http->max_sse_clients) {
        ++http->active_sse_clients;
        acquired = true;
    }
    portEXIT_CRITICAL(&http->state_lock);
    if (!acquired) {
        app_http_server_worker_release(http->http_server);
    }
    return acquired;
}

bool js_http_eval_try_acquire(js_http_t *http)
{
    if (!app_http_server_worker_try_acquire(http->http_server)) {
        return false;
    }
    bool acquired = false;
    portENTER_CRITICAL(&http->state_lock);
    if (!http->stopping &&
        http->active_eval_requests < http->max_eval_requests) {
        ++http->active_eval_requests;
        acquired = true;
    }
    portEXIT_CRITICAL(&http->state_lock);
    if (!acquired) {
        app_http_server_worker_release(http->http_server);
    }
    return acquired;
}

void js_http_eval_release(js_http_t *http)
{
    portENTER_CRITICAL(&http->state_lock);
    if (http->active_eval_requests > 0) {
        --http->active_eval_requests;
    }
    portEXIT_CRITICAL(&http->state_lock);
    app_http_server_worker_release(http->http_server);
}

void js_http_console_release(js_http_t *http)
{
    portENTER_CRITICAL(&http->state_lock);
    if (http->active_sse_clients > 0) {
        --http->active_sse_clients;
    }
    portEXIT_CRITICAL(&http->state_lock);
    app_http_server_worker_release(http->http_server);
}

bool js_http_is_stopping(js_http_t *http)
{
    portENTER_CRITICAL(&http->state_lock);
    bool stopping = http->stopping;
    portEXIT_CRITICAL(&http->state_lock);
    return stopping;
}
