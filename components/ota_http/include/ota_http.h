#pragma once

#include "app_http_server.h"
#include "app_ota.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ota_http ota_http_t;

typedef struct {
    app_http_server_t *http_server;
    app_ota_t *ota;
} ota_http_config_t;

esp_err_t ota_http_start(const ota_http_config_t *config,
                         ota_http_t **out_http);
esp_err_t ota_http_stop(ota_http_t *http, TickType_t wait_ticks);

#ifdef __cplusplus
}
#endif
