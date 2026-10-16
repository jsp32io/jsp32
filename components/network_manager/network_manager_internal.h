#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "network_manager.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#define NETWORK_AP_DEFAULT_IP "192.168.7.1"
#define NETWORK_AP_DEFAULT_NETMASK "255.255.255.0"
#define NETWORK_NTP_SERVER_CAPACITY 254

typedef enum {
    NETWORK_COMMAND_STA_START,
    NETWORK_COMMAND_STA_STOP,
    NETWORK_COMMAND_DISCONNECTED,
    NETWORK_COMMAND_GOT_IP,
    NETWORK_COMMAND_LOST_IP,
    NETWORK_COMMAND_SCAN,
    NETWORK_COMMAND_APPLY,
    NETWORK_COMMAND_TX_POWER,
    NETWORK_COMMAND_STOP,
} network_command_type_t;

typedef struct {
    int8_t power;
    esp_err_t result;
    SemaphoreHandle_t done;
} network_power_request_t;

typedef struct {
    network_command_type_t type;
    uint32_t session;
    union {
        uint32_t reason;
        uint32_t ipv4;
        network_settings_t settings;
        network_power_request_t *power_request;
    } data;
} network_command_t;

struct network_manager {
    app_http_server_t *http_server;
    QueueHandle_t queue;
    SemaphoreHandle_t status_mutex;
    SemaphoreHandle_t startup_ready;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    esp_event_handler_instance_t wifi_event_instance;
    esp_event_handler_instance_t ip_event_instance;
    esp_netif_t *sta_netif;
    esp_netif_t *ap_netif;
    network_status_t status;
    network_settings_t saved;
    network_settings_t candidate;
    network_scan_result_t scan_results[CONFIG_NETWORK_SCAN_MAX_RESULTS];
    size_t scan_result_count;
    uint32_t session;
    unsigned int retry_count;
    int64_t connect_deadline_us;
    int64_t reconnect_at_us;
    int64_t apply_at_us;
    TaskHandle_t dns_task;
    SemaphoreHandle_t dns_stopped;
    int dns_socket;
    uint32_t dns_ip;
    volatile bool dns_stop_requested;
    bool wifi_initialized;
    bool sntp_initialized;
    char ntp_server[NETWORK_NTP_SERVER_CAPACITY];
    bool wifi_started;
    bool sta_started;
    bool apply_ready;
    bool ignore_next_disconnect;
    bool testing;
    bool stopping;
    bool http_registered;
    esp_err_t startup_result;
};

void network_secure_clear(void *data, size_t length);
bool network_credentials_valid(const char *ssid, const char *password);
bool network_settings_valid(const network_settings_t *settings);
void network_store_defaults(network_settings_t *settings);
esp_err_t network_store_load(network_settings_t *settings, bool *mode_saved);
esp_err_t network_store_save(const network_settings_t *settings);
esp_err_t network_store_load_ap_ip(esp_netif_ip_info_t *ip_info);
esp_err_t network_store_load_ntp_server(char server[NETWORK_NTP_SERVER_CAPACITY]);
esp_err_t network_manager_post(network_manager_t *manager,
                               const network_command_t *command,
                               TickType_t wait_ticks);
esp_err_t network_http_start(network_manager_t *manager);
void network_http_stop(network_manager_t *manager);
esp_err_t network_dns_start(network_manager_t *manager);
void network_dns_stop(network_manager_t *manager);

esp_err_t network_manager_set_tx_power(network_manager_t *manager, int8_t *power);

esp_err_t network_manager_apply_deferred(network_manager_t *manager,
                                          const network_settings_t *settings,
                                          uint32_t *attempt_id);
void network_manager_release_apply(network_manager_t *manager);
