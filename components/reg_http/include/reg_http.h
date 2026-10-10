#pragma once

#include "app_http_server.h"
#include "app_registers.h"
#include "esp_err.h"

typedef struct reg_http reg_http_t;

typedef struct {
    app_http_server_t *http_server;
    app_registers_t *registers;
} reg_http_config_t;

esp_err_t reg_http_start(const reg_http_config_t *config, reg_http_t **out_http);
