#pragma once

#include "app_filesystem.h"
#include "app_http_server.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fs_http fs_http_t;

typedef struct {
    app_http_server_t *http_server;
    app_filesystem_t *filesystem;
} fs_http_config_t;

esp_err_t fs_http_start(const fs_http_config_t *config, fs_http_t **out_http);
esp_err_t fs_http_stop(fs_http_t *http, TickType_t wait_ticks);

#ifdef __cplusplus
}
#endif
