#include "network_manager_internal.h"
#include "wifi_tx_power.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_wifi_default.h"

ESP_EVENT_DEFINE_BASE(NETWORK_MANAGER_EVENT);
static const char *TAG = "network_manager";

void network_secure_clear(void *data, size_t length)
{
    volatile unsigned char *bytes = data;
    while (length--) *bytes++ = 0;
}

static void publish_status(network_manager_t *manager, network_event_id_t event)
{
    network_status_t snapshot;
    network_manager_get_status(manager, &snapshot);
    esp_event_post(NETWORK_MANAGER_EVENT, event, &snapshot, sizeof(snapshot), 0);
}

static void update_state(network_manager_t *manager, network_state_t state,
                         esp_err_t error)
{
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->status.state = state;
    manager->status.last_error = error;
    xSemaphoreGive(manager->status_mutex);
    publish_status(manager, NETWORK_EVENT_STATE_CHANGED);
}

static bool connects_station(network_mode_t mode)
{
    return mode == NETWORK_MODE_STA || mode == NETWORK_MODE_APSTA;
}

static esp_err_t configure_ap_ip(esp_netif_t *netif)
{
    esp_netif_ip_info_t ip_info;
    esp_err_t err = network_store_load_ap_ip(&ip_info);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "AP IP configuration: %s; using %s/24", esp_err_to_name(err), NETWORK_AP_DEFAULT_IP);
    err = esp_netif_dhcps_stop(netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
    err = esp_netif_set_ip_info(netif, &ip_info);
    if (err != ESP_OK) return err;
    /* Before Wi-Fi starts this restores DHCP's automatic-start state. */
    return esp_netif_dhcps_start(netif);
}

static const network_settings_t *working_settings(network_manager_t *manager)
{
    return manager->testing ? &manager->candidate : &manager->saved;
}

static esp_err_t connect_station(network_manager_t *manager)
{
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        manager->connect_deadline_us = esp_timer_get_time() +
            (int64_t)CONFIG_NETWORK_CONNECT_TIMEOUT_MS * 1000;
        manager->reconnect_at_us = 0;
    }
    return err;
}

/* All driver mutations run in this task, including stop/start and retry timers. */
static esp_err_t activate(network_manager_t *manager,
                          const network_settings_t *settings, network_mode_t mode)
{
    manager->connect_deadline_us = 0;
    manager->reconnect_at_us = 0;
    manager->sta_started = false;
    manager->ignore_next_disconnect = false;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    ++manager->session;
    manager->status.has_ipv4 = false;
    manager->status.ipv4 = 0;
    manager->status.scanning = false;
    xSemaphoreGive(manager->status_mutex);
    if (manager->wifi_started) {
        esp_wifi_scan_stop();
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) return err;
        manager->wifi_started = false;
    }
    network_dns_stop(manager);
    /* AP/APSTA share the APSTA driver. AP policy never calls connect(). */
    wifi_mode_t wifi_mode = mode == NETWORK_MODE_STA ? WIFI_MODE_STA : WIFI_MODE_APSTA;
    esp_err_t err = esp_wifi_set_mode(wifi_mode);
    if (err == ESP_OK && mode != NETWORK_MODE_STA) {
        wifi_config_t ap = {0};
        memcpy(ap.ap.ssid, settings->ap_ssid, strlen(settings->ap_ssid));
        ap.ap.ssid_len = strlen(settings->ap_ssid);
        strlcpy((char *)ap.ap.password, settings->ap_password, sizeof(ap.ap.password));
        ap.ap.authmode = settings->ap_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        ap.ap.channel = 1;
        ap.ap.max_connection = CONFIG_NETWORK_AP_MAX_CONNECTIONS;
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
        network_secure_clear(&ap, sizeof(ap));
    }
    if (err == ESP_OK && connects_station(mode)) {
        wifi_config_t sta = {0};
        /* Copy all 32 SSID / 64 PSK bytes: driver fields need not be terminated. */
        memcpy(sta.sta.ssid, settings->sta_ssid, strlen(settings->sta_ssid));
        memcpy(sta.sta.password, settings->sta_password, strlen(settings->sta_password));
        sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        sta.sta.threshold.authmode = settings->sta_password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta.sta.pmf_cfg.capable = true;
        err = esp_wifi_set_config(WIFI_IF_STA, &sta);
        network_secure_clear(&sta, sizeof(sta));
    }
    if (err != ESP_OK) return err;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->status.active_mode = mode;
    xSemaphoreGive(manager->status_mutex);
    err = esp_wifi_start();
    if (err != ESP_OK) return err;
    manager->wifi_started = true;
    int8_t power = CONFIG_NETWORK_MANAGER_MAX_TX_POWER_QDBM;
    esp_err_t load_err = wifi_tx_power_load(&power);
    if (load_err != ESP_OK) ESP_LOGW(TAG, "TX limit load: %s", esp_err_to_name(load_err));
    err = esp_wifi_set_max_tx_power(power);
    if (err != ESP_OK) return err;
    if (mode != NETWORK_MODE_STA) {
        err = network_dns_start(manager);
        if (err != ESP_OK) ESP_LOGW(TAG, "Captive DNS unavailable: %s", esp_err_to_name(err));
        /* DNS guidance is optional; the UI remains reachable by IP. */
    }
    return ESP_OK;
}

static void fallback_to_ap(network_manager_t *manager, esp_err_t error)
{
    manager->testing = false;
    manager->apply_at_us = 0;
    network_secure_clear(&manager->candidate, sizeof(manager->candidate));
    esp_err_t activate_err = activate(manager, &manager->saved, NETWORK_MODE_AP);
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->status.pending = false;
    manager->status.fallback = true;
    xSemaphoreGive(manager->status_mutex);
    update_state(manager, activate_err == ESP_OK ? NETWORK_STATE_FALLBACK : NETWORK_STATE_ERROR,
                 activate_err == ESP_OK ? error : activate_err);
    publish_status(manager, NETWORK_EVENT_ERROR);
}

static void retry_or_fallback(network_manager_t *manager, esp_err_t error)
{
    if (manager->testing) {
        fallback_to_ap(manager, error);
        return;
    }
    if (++manager->retry_count > CONFIG_NETWORK_MAX_RETRIES) {
        if (manager->status.active_mode == NETWORK_MODE_STA) {
            fallback_to_ap(manager, error);
        } else {
            manager->connect_deadline_us = 0;
            manager->reconnect_at_us = 0;
            update_state(manager, NETWORK_STATE_ERROR, error);
            publish_status(manager, NETWORK_EVENT_ERROR);
        }
        return;
    }
    unsigned int shift = manager->retry_count - 1;
    if (shift > 4) shift = 4;
    manager->connect_deadline_us = 0;
    manager->reconnect_at_us = esp_timer_get_time() + ((int64_t)1 << shift) * 1000000;
    update_state(manager, NETWORK_STATE_STA_CONNECTING, error);
}

static void commit_candidate(network_manager_t *manager)
{
    esp_err_t err = network_store_save(&manager->candidate);
    if (err != ESP_OK) {
        fallback_to_ap(manager, err);
        return;
    }
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->saved = manager->candidate;
    manager->status.saved_mode = manager->saved.mode;
    manager->status.mode_saved = true;
    manager->status.pending = false;
    manager->status.fallback = false;
    xSemaphoreGive(manager->status_mutex);
    manager->testing = false;
    network_secure_clear(&manager->candidate, sizeof(manager->candidate));
    publish_status(manager, NETWORK_EVENT_CONFIG_SAVED);
}

static void begin_apply(network_manager_t *manager)
{
    manager->apply_at_us = 0;
    manager->testing = true;
    manager->retry_count = 0;
    update_state(manager, NETWORK_STATE_APPLYING, ESP_OK);
    esp_err_t err = activate(manager, &manager->candidate, manager->candidate.mode);
    if (err != ESP_OK) {
        fallback_to_ap(manager, err);
        return;
    }
    if (connects_station(manager->candidate.mode)) {
        update_state(manager, NETWORK_STATE_STA_CONNECTING, ESP_OK);
    } else {
        commit_candidate(manager);
        if (!manager->status.fallback) update_state(manager, NETWORK_STATE_AP_READY, ESP_OK);
    }
}

static void handle_got_ip(network_manager_t *manager, uint32_t ipv4)
{
    if (!manager->sta_started || !connects_station(manager->status.active_mode) ||
        (manager->status.state != NETWORK_STATE_STA_CONNECTING &&
         manager->status.state != NETWORK_STATE_ONLINE)) return;
    wifi_ap_record_t ap = {0};
    esp_netif_ip_info_t info;
    const char *expected = working_settings(manager)->sta_ssid;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        strnlen(expected, 33) != strnlen((const char *)ap.ssid, sizeof(ap.ssid)) ||
        memcmp(ap.ssid, expected, strlen(expected)) != 0 ||
        esp_netif_get_ip_info(manager->sta_netif, &info) != ESP_OK ||
        info.ip.addr != ipv4) return;
    if (manager->testing) {
        commit_candidate(manager);
        if (manager->status.fallback) return;
    }
    manager->retry_count = 0;
    manager->connect_deadline_us = 0;
    manager->reconnect_at_us = 0;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->status.has_ipv4 = true;
    manager->status.ipv4 = ipv4;
    manager->status.disconnect_reason = 0;
    manager->status.fallback = false;
    xSemaphoreGive(manager->status_mutex);
    update_state(manager, NETWORK_STATE_ONLINE, ESP_OK);
    esp_err_t time_error;
    if (manager->sntp_initialized) {
        time_error = esp_netif_sntp_start();
    } else {
        esp_err_t config_error = network_store_load_ntp_server(manager->ntp_server);
        if (config_error != ESP_OK)
            ESP_LOGW(TAG, "Invalid NTP configuration; using default: %s", esp_err_to_name(config_error));
        esp_sntp_config_t time_config = ESP_NETIF_SNTP_DEFAULT_CONFIG(manager->ntp_server);
        time_config.wait_for_sync = false;
        time_error = esp_netif_sntp_init(&time_config);
        manager->sntp_initialized = time_error == ESP_OK;
    }
    if (time_error != ESP_OK)
        ESP_LOGW(TAG, "Time synchronization unavailable: %s", esp_err_to_name(time_error));
    publish_status(manager, NETWORK_EVENT_ONLINE);
}

static int compare_scan(const void *left, const void *right)
{
    return ((const network_scan_result_t *)right)->rssi -
           ((const network_scan_result_t *)left)->rssi;
}

static esp_err_t collect_scan(network_manager_t *manager)
{
    uint16_t count = 0;
    esp_err_t err = esp_wifi_scan_get_ap_num(&count);
    if (count > CONFIG_NETWORK_SCAN_MAX_RESULTS * 3) count = CONFIG_NETWORK_SCAN_MAX_RESULTS * 3;
    wifi_ap_record_t *records = count ? calloc(count, sizeof(*records)) : NULL;
    if (count && !records) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && count) err = esp_wifi_scan_get_ap_records(&count, records);
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->scan_result_count = 0;
    if (err == ESP_OK) {
        for (uint16_t i = 0; i < count; ++i) {
            if (!records[i].ssid[0]) continue;
            char ssid[33] = {0};
            memcpy(ssid, records[i].ssid, 32);
            bool duplicate = false;
            for (size_t j = 0; j < manager->scan_result_count; ++j) {
                if (strcmp(manager->scan_results[j].ssid, ssid) == 0) { duplicate = true; break; }
            }
            if (duplicate || manager->scan_result_count == CONFIG_NETWORK_SCAN_MAX_RESULTS) continue;
            network_scan_result_t *result = &manager->scan_results[manager->scan_result_count++];
            strlcpy(result->ssid, ssid, sizeof(result->ssid));
            result->rssi = records[i].rssi;
            result->channel = records[i].primary;
            result->authmode = records[i].authmode;
        }
        qsort(manager->scan_results, manager->scan_result_count, sizeof(manager->scan_results[0]), compare_scan);
    }
    xSemaphoreGive(manager->status_mutex);
    esp_wifi_clear_ap_list();
    free(records);
    return err;
}


static void scan_networks(network_manager_t *manager)
{
    /* HTTP remains asynchronous; the worker serializes scan completion with
       other network operations. All policies keep the STA scanner available. */
    esp_err_t err = esp_wifi_scan_start(NULL, true);
    if (err == ESP_OK) err = collect_scan(manager);
    if (err != ESP_OK) {
        esp_wifi_clear_ap_list();
        xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
        manager->scan_result_count = 0;
        xSemaphoreGive(manager->status_mutex);
    }
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->status.scanning = false;
    manager->status.last_error = err;
    xSemaphoreGive(manager->status_mutex);
    publish_status(manager, NETWORK_EVENT_STATE_CHANGED);
}


static esp_err_t set_tx_power(int8_t *power)
{
    int8_t previous;
    esp_err_t err = esp_wifi_get_max_tx_power(&previous);
    if (err != ESP_OK) return err;
    /* Save before raising TX power: higher power can disrupt Flash on this board. */
    err = wifi_tx_power_save(*power);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_max_tx_power(*power);
    if (err == ESP_OK) err = esp_wifi_get_max_tx_power(power);
    if (err != ESP_OK) {
        esp_wifi_set_max_tx_power(previous);
        esp_err_t restore_err = wifi_tx_power_save(previous);
        if (restore_err != ESP_OK) return restore_err;
    }
    return err;
}

static void handle_command(network_manager_t *manager, const network_command_t *command)
{
    if (command->type <= NETWORK_COMMAND_LOST_IP && command->session != manager->session) return;
    switch (command->type) {
    case NETWORK_COMMAND_STA_START:
        if (connects_station(manager->status.active_mode)) {
            manager->sta_started = true;
            if (manager->status.state == NETWORK_STATE_STA_CONNECTING) {
                esp_err_t err = connect_station(manager);
                if (err != ESP_OK) retry_or_fallback(manager, err);
            }
        }
        break;
    case NETWORK_COMMAND_STA_STOP:
        manager->sta_started = false;
        break;
    case NETWORK_COMMAND_DISCONNECTED:
        if (!manager->sta_started || !connects_station(manager->status.active_mode)) break;
        xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
        manager->status.has_ipv4 = false;
        manager->status.ipv4 = 0;
        manager->status.disconnect_reason = command->data.reason;
        xSemaphoreGive(manager->status_mutex);
        if (manager->ignore_next_disconnect) {
            manager->ignore_next_disconnect = false;
            break;
        }
        if (!manager->status.pending || manager->testing) retry_or_fallback(manager, ESP_FAIL);
        break;
    case NETWORK_COMMAND_GOT_IP:
        handle_got_ip(manager, command->data.ipv4);
        break;
    case NETWORK_COMMAND_LOST_IP:
        if (manager->sta_started && connects_station(manager->status.active_mode)) {
            xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
            manager->status.has_ipv4 = false;
            manager->status.ipv4 = 0;
            xSemaphoreGive(manager->status_mutex);
            manager->connect_deadline_us = esp_timer_get_time() +
                (int64_t)CONFIG_NETWORK_CONNECT_TIMEOUT_MS * 1000;
            update_state(manager, NETWORK_STATE_STA_CONNECTING, ESP_FAIL);
        }
        break;
    case NETWORK_COMMAND_SCAN:
        scan_networks(manager);
        break;
    case NETWORK_COMMAND_APPLY:
        manager->candidate = command->data.settings;
        /* Let the HTTP response and reconnect instructions reach the client. */
        manager->apply_at_us = esp_timer_get_time() + 1000000;
        break;
    case NETWORK_COMMAND_TX_POWER: {
        network_power_request_t *request = command->data.power_request;
        request->result = set_tx_power(&request->power);
        xSemaphoreGive(request->done);
        break;
    }
    case NETWORK_COMMAND_STOP:
        manager->stopping = true;
        break;
    }
}

static void handle_timers(network_manager_t *manager)
{
    int64_t now = esp_timer_get_time();
    if (manager->apply_at_us) {
        xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
        bool ready = manager->apply_ready;
        xSemaphoreGive(manager->status_mutex);
        if (ready && now >= manager->apply_at_us) begin_apply(manager);
        return;
    }
    if (manager->reconnect_at_us && now >= manager->reconnect_at_us) {
        manager->reconnect_at_us = 0;
        esp_err_t err = connect_station(manager);
        if (err != ESP_OK) retry_or_fallback(manager, err);
    }
    if (manager->connect_deadline_us && now >= manager->connect_deadline_us) {
        manager->connect_deadline_us = 0;
        /* Stop the pending association before retrying; AP remains up in APSTA. */
        manager->ignore_next_disconnect = true;
        if (esp_wifi_disconnect() != ESP_OK) manager->ignore_next_disconnect = false;
        retry_or_fallback(manager, ESP_ERR_TIMEOUT);
    }
}

static void manager_task(void *argument)
{
    network_manager_t *manager = argument;
    bool mode_saved = false;
    esp_err_t load_err = network_store_load(&manager->saved, &mode_saved);
    if (load_err != ESP_OK) ESP_LOGW(TAG, "Network settings load: %s; using open AP", esp_err_to_name(load_err));
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->status.saved_mode = manager->saved.mode;
    manager->status.mode_saved = mode_saved;
    xSemaphoreGive(manager->status_mutex);
    esp_err_t err = activate(manager, &manager->saved, manager->saved.mode);
    if (err == ESP_OK) update_state(manager, connects_station(manager->saved.mode) ?
                                    NETWORK_STATE_STA_CONNECTING : NETWORK_STATE_AP_READY, load_err);
    manager->startup_result = err;
    xSemaphoreGive(manager->startup_ready);
    while (err == ESP_OK && !manager->stopping) {
        network_command_t command;
        if (xQueueReceive(manager->queue, &command, pdMS_TO_TICKS(100)) == pdTRUE) {
            handle_command(manager, &command);
            network_secure_clear(&command, sizeof(command));
        }
        if (!manager->stopping) handle_timers(manager);
    }
    update_state(manager, NETWORK_STATE_STOPPING, ESP_OK);
    network_dns_stop(manager);
    if (manager->wifi_started) esp_wifi_stop();
    manager->wifi_started = false;
    network_secure_clear(&manager->candidate, sizeof(manager->candidate));
    manager->task = NULL;
    xSemaphoreGive(manager->stopped);
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    network_manager_t *manager = argument;
    network_command_t command = {0};
    if (id == WIFI_EVENT_STA_START) command.type = NETWORK_COMMAND_STA_START;
    else if (id == WIFI_EVENT_STA_STOP) command.type = NETWORK_COMMAND_STA_STOP;
    else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        command.type = NETWORK_COMMAND_DISCONNECTED;
        command.data.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
    } else return;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    command.session = manager->session;
    xSemaphoreGive(manager->status_mutex);
    network_manager_post(manager, &command, 0);
}

static void ip_event_handler(void *argument, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP && id != IP_EVENT_STA_LOST_IP) return;
    network_manager_t *manager = argument;
    network_command_t command = {0};
    if (id == IP_EVENT_STA_GOT_IP) {
        command.type = NETWORK_COMMAND_GOT_IP;
        command.data.ipv4 = ((ip_event_got_ip_t *)data)->ip_info.ip.addr;
    } else command.type = NETWORK_COMMAND_LOST_IP;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    command.session = manager->session;
    xSemaphoreGive(manager->status_mutex);
    network_manager_post(manager, &command, 0);
}

esp_err_t network_manager_post(network_manager_t *manager, const network_command_t *command, TickType_t wait_ticks)
{
    if (!manager || !command || !manager->queue) return ESP_ERR_INVALID_ARG;
    return xQueueSend(manager->queue, command, wait_ticks) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_manager_get_status(network_manager_t *manager, network_status_t *out_status)
{
    if (!manager || !out_status || !manager->status_mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    *out_status = manager->status;
    xSemaphoreGive(manager->status_mutex);
    return ESP_OK;
}

esp_err_t network_manager_get_settings(network_manager_t *manager, network_settings_t *out_settings)
{
    if (!manager || !out_settings || !manager->status_mutex) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    *out_settings = manager->saved;
    xSemaphoreGive(manager->status_mutex);
    return ESP_OK;
}

static esp_err_t submit_apply(network_manager_t *manager, const network_settings_t *settings, uint32_t *attempt_id, bool defer)
{
    if (!manager || !attempt_id || !network_settings_valid(settings)) return ESP_ERR_INVALID_ARG;
    network_command_t command = {.type = NETWORK_COMMAND_APPLY, .data.settings = *settings};
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    if (!manager->task || manager->status.pending || manager->status.scanning) {
        xSemaphoreGive(manager->status_mutex);
        network_secure_clear(&command, sizeof(command));
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = network_manager_post(manager, &command, 0);
    if (err == ESP_OK) {
        manager->status.pending = true;
        manager->apply_ready = !defer;
        manager->status.last_error = ESP_OK;
        *attempt_id = ++manager->status.attempt_id;
    }
    xSemaphoreGive(manager->status_mutex);
    network_secure_clear(&command, sizeof(command));
    return err;
}

esp_err_t network_manager_scan(network_manager_t *manager)
{
    if (!manager) return ESP_ERR_INVALID_ARG;
    network_command_t command = {.type = NETWORK_COMMAND_SCAN};
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    if (!manager->task || !manager->status.active_mode ||
        manager->status.pending || manager->status.scanning ||
        manager->status.state == NETWORK_STATE_STA_CONNECTING) {
        xSemaphoreGive(manager->status_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = network_manager_post(manager, &command, 0);
    if (err == ESP_OK) {
        manager->status.scanning = true;
        manager->status.last_error = ESP_OK;
    }
    xSemaphoreGive(manager->status_mutex);
    return err;
}

size_t network_manager_copy_scan_results(network_manager_t *manager, network_scan_result_t *results, size_t capacity)
{
    if (!manager || !results) return 0;
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    size_t count = manager->scan_result_count < capacity ? manager->scan_result_count : capacity;
    memcpy(results, manager->scan_results, count * sizeof(*results));
    xSemaphoreGive(manager->status_mutex);
    return count;
}

esp_err_t network_manager_init(const network_manager_config_t *config, network_manager_t **out_manager)
{
    if (!config || !config->http_server || !out_manager) return ESP_ERR_INVALID_ARG;
    network_manager_t *manager = calloc(1, sizeof(*manager));
    if (!manager) return ESP_ERR_NO_MEM;
    manager->http_server = config->http_server;
    manager->dns_socket = -1;
    manager->queue = xQueueCreate(16, sizeof(network_command_t));
    manager->status_mutex = xSemaphoreCreateMutex();
    manager->startup_ready = xSemaphoreCreateBinary();
    manager->stopped = xSemaphoreCreateBinary();
    if (!manager->queue || !manager->status_mutex || !manager->startup_ready || !manager->stopped) {
        network_manager_stop(manager);
        return ESP_ERR_NO_MEM;
    }
    manager->sta_netif = esp_netif_create_default_wifi_sta();
    manager->ap_netif = esp_netif_create_default_wifi_ap();
    esp_err_t err = manager->sta_netif && manager->ap_netif ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = configure_ap_ip(manager->ap_netif);
    if (err == ESP_OK) {
        wifi_init_config_t wifi = WIFI_INIT_CONFIG_DEFAULT();
        wifi.nvs_enable = false;
        err = esp_wifi_init(&wifi);
        manager->wifi_initialized = err == ESP_OK;
    }
    if (err == ESP_OK) err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err == ESP_OK) err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                                wifi_event_handler, manager, &manager->wifi_event_instance);
    if (err == ESP_OK) err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                                                ip_event_handler, manager, &manager->ip_event_instance);
    if (err != ESP_OK) { network_manager_stop(manager); return err; }
    *out_manager = manager;
    return ESP_OK;
}

esp_err_t network_manager_start(network_manager_t *manager)
{
    if (!manager || manager->task) return ESP_ERR_INVALID_STATE;
    if (xTaskCreate(manager_task, "network_manager", CONFIG_NETWORK_TASK_STACK_SIZE, manager, 5,
                    &manager->task) != pdPASS) return ESP_ERR_NO_MEM;
    xSemaphoreTake(manager->startup_ready, portMAX_DELAY);
    if (manager->startup_result != ESP_OK) return manager->startup_result;
    esp_err_t err = network_http_start(manager);
    manager->http_registered = err == ESP_OK;
    return err;
}

esp_err_t network_manager_stop(network_manager_t *manager)
{
    if (!manager) return ESP_ERR_INVALID_ARG;
    if (manager->http_registered) network_http_stop(manager);
    if (manager->task) {
        network_command_t command = {.type = NETWORK_COMMAND_STOP};
        network_manager_post(manager, &command, portMAX_DELAY);
        xSemaphoreTake(manager->stopped, portMAX_DELAY);
    }
    if (manager->wifi_event_instance) esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, manager->wifi_event_instance);
    if (manager->ip_event_instance) esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, manager->ip_event_instance);
    if (manager->sntp_initialized) esp_netif_sntp_deinit();
    if (manager->wifi_initialized) esp_wifi_deinit();
    if (manager->sta_netif) esp_netif_destroy_default_wifi(manager->sta_netif);
    if (manager->ap_netif) esp_netif_destroy_default_wifi(manager->ap_netif);
    if (manager->queue) vQueueDelete(manager->queue);
    if (manager->status_mutex) vSemaphoreDelete(manager->status_mutex);
    if (manager->startup_ready) vSemaphoreDelete(manager->startup_ready);
    if (manager->stopped) vSemaphoreDelete(manager->stopped);
    if (manager->dns_stopped) vSemaphoreDelete(manager->dns_stopped);
    network_secure_clear(manager, sizeof(*manager));
    free(manager);
    return ESP_OK;
}

esp_err_t network_manager_set_tx_power(network_manager_t *manager, int8_t *power)
{
    if (!manager || !power || *power < 8 || *power > 84) return ESP_ERR_INVALID_ARG;
    if (!manager->task) return ESP_ERR_INVALID_STATE;
    network_power_request_t request = {.power = *power, .done = xSemaphoreCreateBinary()};
    if (!request.done) return ESP_ERR_NO_MEM;
    network_command_t command = {.type = NETWORK_COMMAND_TX_POWER, .data.power_request = &request};
    esp_err_t err = network_manager_post(manager, &command, 0);
    if (err == ESP_OK) {
        xSemaphoreTake(request.done, portMAX_DELAY);
        err = request.result;
        if (err == ESP_OK) *power = request.power;
    }
    vSemaphoreDelete(request.done);
    return err;
}

esp_err_t network_manager_apply(network_manager_t *manager,
                                const network_settings_t *settings,
                                uint32_t *attempt_id)
{
    return submit_apply(manager, settings, attempt_id, false);
}

esp_err_t network_manager_apply_deferred(network_manager_t *manager,
                                         const network_settings_t *settings,
                                         uint32_t *attempt_id)
{
    return submit_apply(manager, settings, attempt_id, true);
}

void network_manager_release_apply(network_manager_t *manager)
{
    xSemaphoreTake(manager->status_mutex, portMAX_DELAY);
    manager->apply_ready = true;
    xSemaphoreGive(manager->status_mutex);
}
