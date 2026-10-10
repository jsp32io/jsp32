#pragma once
#include "app_http_server.h"

/* Firmware-embedded device UI; communicates with modules through HTTP APIs. */
esp_err_t app_ui_start(app_http_server_t *server);
