#pragma once

#include "app_http_server.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "js_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_http js_http_t;

typedef struct {
    app_http_server_t *http_server;
    js_runtime_t *runtime;
} js_http_config_t;

esp_err_t js_http_start(const js_http_config_t *config,
                        js_http_t **out_http);
esp_err_t js_http_stop(js_http_t *http, TickType_t wait_ticks);

#ifdef __cplusplus
}
#endif
