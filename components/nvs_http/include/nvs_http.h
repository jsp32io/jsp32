#pragma once

#include "app_http_server.h"
#include "app_nvs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nvs_http nvs_http_t;

typedef struct {
    app_http_server_t *http_server;
    app_nvs_t *store;
} nvs_http_config_t;

esp_err_t nvs_http_start(const nvs_http_config_t *config,
                         nvs_http_t **out_http);
esp_err_t nvs_http_stop(nvs_http_t *http, TickType_t wait_ticks);

#ifdef __cplusplus
}
#endif
