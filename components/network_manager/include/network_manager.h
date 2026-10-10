#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "app_http_server.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(NETWORK_MANAGER_EVENT);
typedef struct network_manager network_manager_t;

/* Connection policy: AP uses the APSTA driver for scanning, without STA connection. */
typedef enum {
    NETWORK_MODE_AP = 1,
    NETWORK_MODE_STA = 2,
    NETWORK_MODE_APSTA = 3,
} network_mode_t;

typedef enum {
    NETWORK_STATE_INIT,
    NETWORK_STATE_AP_READY,
    NETWORK_STATE_STA_CONNECTING,
    NETWORK_STATE_ONLINE,
    NETWORK_STATE_APPLYING,
    NETWORK_STATE_FALLBACK,
    NETWORK_STATE_ERROR,
    NETWORK_STATE_STOPPING,
} network_state_t;

typedef enum {
    NETWORK_EVENT_STATE_CHANGED,
    NETWORK_EVENT_ONLINE,
    NETWORK_EVENT_CONFIG_SAVED,
    NETWORK_EVENT_ERROR,
} network_event_id_t;

/* Passwords are write-only over HTTP. Empty AP password means an open AP. */
typedef struct {
    network_mode_t mode;
    char ap_ssid[33];
    char ap_password[64];
    char sta_ssid[33];
    char sta_password[65];
} network_settings_t;

typedef struct {
    network_state_t state;
    network_mode_t active_mode;
    network_mode_t saved_mode;
    bool mode_saved;
    bool pending;
    bool scanning;
    bool has_ipv4;
    bool fallback;
    uint32_t ipv4;
    uint32_t attempt_id;
    uint32_t disconnect_reason;
    esp_err_t last_error;
} network_status_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t authmode;
    uint8_t channel;
} network_scan_result_t;

typedef struct { app_http_server_t *http_server; } network_manager_config_t;

esp_err_t network_manager_init(const network_manager_config_t *config,
                               network_manager_t **out_manager);
esp_err_t network_manager_start(network_manager_t *manager);
esp_err_t network_manager_get_status(network_manager_t *manager,
                                     network_status_t *out_status);
esp_err_t network_manager_get_settings(network_manager_t *manager,
                                       network_settings_t *out_settings);
esp_err_t network_manager_apply(network_manager_t *manager,
                                const network_settings_t *settings,
                                uint32_t *attempt_id);
esp_err_t network_manager_scan(network_manager_t *manager);
size_t network_manager_copy_scan_results(network_manager_t *manager,
                                         network_scan_result_t *results,
                                         size_t capacity);
esp_err_t network_manager_stop(network_manager_t *manager);

#ifdef __cplusplus
}
#endif
