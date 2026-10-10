#pragma once
#include "mqtt_report.h"
#include "mqtt_client.h"
#include "freertos/semphr.h"
#include <stdatomic.h>

typedef struct {
    bool enabled;
    char url[257], username[129], password[129];
} mqtt_broker_config_t;
typedef struct {
    mqtt_broker_config_t config;
    esp_mqtt_client_handle_t client;
    atomic_int state; /* 0 disabled, 1 connecting, 2 connected, 3 disconnected, 4 error */
    atomic_uint generation;
    unsigned observed_generation;
    atomic_uint queued, rejected;
} mqtt_broker_t;
struct app_mqtt {
    app_registers_t *registers;
    SemaphoreHandle_t lock;
    mqtt_report_t *reports;
    uint32_t subscription;
    char device_id[13];
    mqtt_broker_t broker;
};
esp_err_t mqtt_config_load(mqtt_broker_config_t *config);
bool mqtt_config_valid(const mqtt_broker_config_t *config);
esp_err_t mqtt_config_save(const mqtt_broker_config_t *config);
esp_err_t mqtt_http_register(app_mqtt_t *service, app_http_server_t *http);
