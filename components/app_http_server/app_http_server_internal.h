#pragma once

#include "app_http_server.h"

esp_err_t system_info_register(app_http_server_t *server);

esp_err_t man_register(app_http_server_t *server);

struct cJSON;
void app_http_server_append_info(app_http_server_t *server, struct cJSON *root);
