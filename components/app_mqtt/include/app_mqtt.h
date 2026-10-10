#pragma once

#include "app_http_server.h"
#include "app_registers.h"

#define APP_MQTT_MAX_REPORTS 32
#define APP_MQTT_MAX_FIELDS 64
#define APP_MQTT_MAX_PAYLOAD 8192

typedef struct app_mqtt app_mqtt_t;

/* Service and store outlive JS runtimes. Config changes take effect after reboot. */
esp_err_t app_mqtt_start(app_registers_t *registers, app_http_server_t *http,
                         app_mqtt_t **out);
esp_err_t app_mqtt_register_report(app_mqtt_t *service, const char *plugin_id,
                                   const char *json, char *error, size_t error_size);
esp_err_t app_mqtt_validate_report(app_mqtt_t *service, const char *plugin_id,
                                   const char *json, char *error, size_t error_size);
bool app_mqtt_unregister_report(app_mqtt_t *service, const char *plugin_id);
void app_mqtt_clear_reports(app_mqtt_t *service);
