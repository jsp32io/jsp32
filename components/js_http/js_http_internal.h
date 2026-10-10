#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "js_http.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define JS_HTTP_OWNER "js_http"

struct js_http {
    app_http_server_t *http_server;
    js_runtime_t *runtime;
    portMUX_TYPE state_lock;
    uint32_t active_sse_clients;
    uint32_t active_eval_requests;
    uint32_t max_sse_clients;
    uint32_t max_eval_requests;
    bool stopping;
};

esp_err_t js_http_console_register(js_http_t *http);
esp_err_t js_http_eval_register(js_http_t *http);
bool js_http_console_try_acquire(js_http_t *http);
void js_http_console_release(js_http_t *http);
bool js_http_eval_try_acquire(js_http_t *http);
void js_http_eval_release(js_http_t *http);
bool js_http_is_stopping(js_http_t *http);
