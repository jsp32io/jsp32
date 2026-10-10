#pragma once
#include "app_mqtt.h"
#include "cJSON.h"

typedef enum { MQTT_BOOLEAN, MQTT_U16, MQTT_I16, MQTT_U32, MQTT_I32, MQTT_F32 } mqtt_value_type_t;
typedef struct {
    char name[64];
    app_register_address_t address;
    mqtt_value_type_t type;
    bool low_first, trigger;
    double scale, offset;
} mqtt_field_t;

typedef struct mqtt_report {
    char id[33], topic[257];
    bool change_mode, retain, pending;
    int qos;
    uint32_t interval_ms, min_interval_ms, heartbeat_ms;
    uint64_t next_interval_ms, last_ms, retry_after_ms;
    size_t field_count;
    mqtt_field_t fields[APP_MQTT_MAX_FIELDS];
    struct mqtt_report *next;
} mqtt_report_t;

esp_err_t mqtt_report_parse(const char *id, const char *device_id, const char *json,
                             mqtt_report_t **out, char *error, size_t error_size);
bool mqtt_report_matches(const mqtt_report_t *report, app_register_address_t address);
/* Returns heap JSON, or NULL on read, encoding, or payload limit failure. */
char *mqtt_report_encode(const mqtt_report_t *report, app_registers_t *registers,
                          uint64_t uptime_ms);
bool mqtt_report_due(mqtt_report_t *report, uint64_t now_ms, bool connected);
void mqtt_report_sent(mqtt_report_t *report, uint64_t now_ms);
