#include "mqtt_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "nvs.h"

static const char *TAG = "app_mqtt";

bool mqtt_config_valid(const mqtt_broker_config_t *c)
{
    if (!c->url[0]) return !c->enabled;
    const char *host;
    if (!strncmp(c->url, "mqtt://", 7)) host = c->url + 7;
    else if (!strncmp(c->url, "mqtts://", 8)) host = c->url + 8;
    else return false;
    return *host && *host != '/' && !strpbrk(host, "@ \t\r\n?#");
}

static void read_string(nvs_handle_t nvs, const char *key, char *out, size_t capacity)
{
    size_t size = capacity;
    esp_err_t err = nvs_get_str(nvs, key, out, &size);
    if (err != ESP_OK) {
        out[0] = 0;
        if (err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGW(TAG, "Invalid NVS key %s", key);
    }
}

esp_err_t mqtt_config_load(mqtt_broker_config_t *config)
{
    memset(config, 0, sizeof(*config));
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("jsp32_config", NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t enabled = 0;
    err = nvs_get_u8(nvs, "mqtt_enabled", &enabled);
    if ((err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) || enabled > 1)
        ESP_LOGW(TAG, "Invalid NVS key mqtt_enabled");
    config->enabled = err == ESP_OK && enabled == 1;
    read_string(nvs, "mqtt_url", config->url, sizeof(config->url));
    read_string(nvs, "mqtt_username", config->username, sizeof(config->username));
    read_string(nvs, "mqtt_password", config->password, sizeof(config->password));
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t mqtt_config_save(const mqtt_broker_config_t *config)
{
    if (!mqtt_config_valid(config)) return ESP_ERR_INVALID_ARG;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("jsp32_config", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(nvs, "mqtt_enabled", config->enabled);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_url", config->url);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_username", config->username);
    if (err == ESP_OK) err = nvs_set_str(nvs, "mqtt_password", config->password);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static void broker_event(void *opaque, esp_event_base_t base, int32_t id, void *data)
{
    (void)base; (void)data;
    mqtt_broker_t *b = opaque;
    if (id == MQTT_EVENT_CONNECTED) {
        atomic_fetch_add(&b->generation, 1);
        atomic_store(&b->state, 2);
    } else if (id == MQTT_EVENT_DISCONNECTED) atomic_store(&b->state, 3);
    else if (id == MQTT_EVENT_ERROR) atomic_store(&b->state, 4);
    else if (id == MQTT_EVENT_BEFORE_CONNECT) atomic_store(&b->state, 1);
}

static void registers_changed(const app_register_event_t *event, void *opaque)
{
    app_mqtt_t *s = opaque;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    for (mqtt_report_t *r = s->reports; r; r = r->next) {
        if (!r->change_mode) continue;
        for (size_t i = 0; i < event->change_count; ++i) {
            if (mqtt_report_matches(r, event->changes[i].address)) {
                r->pending = true;
                break;
            }
        }
    }
    xSemaphoreGive(s->lock);
}

static void report_task(void *opaque)
{
    app_mqtt_t *s = opaque;
    for (;;) {
        uint64_t now = esp_timer_get_time() / 1000;
        xSemaphoreTake(s->lock, portMAX_DELAY);
        mqtt_broker_t *broker = &s->broker;
        bool connected = atomic_load(&broker->state) == 2;
        unsigned generation = atomic_load(&broker->generation);
        bool reconnected = connected && broker->observed_generation != generation;
        if (reconnected) broker->observed_generation = generation;
        for (mqtt_report_t *r = s->reports; r; r = r->next) {
            if (reconnected) { r->pending = true; r->last_ms = 0; r->retry_after_ms = 0; }
            if (!mqtt_report_due(r, now, connected)) continue;
            char *json = mqtt_report_encode(r, s->registers, now);
            if (!json) continue;
            /* No network waits here; the bounded outbox is sent by ESP-MQTT. */
            int result = esp_mqtt_client_enqueue(broker->client, r->topic, json, 0,
                                                   r->qos, r->retain, true);
            if (result >= 0) {
                mqtt_report_sent(r, now);
                atomic_fetch_add(&broker->queued, 1);
            } else {
                atomic_fetch_add(&broker->rejected, 1);
                r->retry_after_ms = now + 1000;
            }
            cJSON_free(json);
        }
        xSemaphoreGive(s->lock);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t app_mqtt_validate_report(app_mqtt_t *s, const char *id, const char *json,
                                   char *error, size_t error_size)
{
    if (!s) return ESP_ERR_INVALID_STATE;
    mqtt_report_t *report = NULL;
    esp_err_t err = mqtt_report_parse(id, s->device_id, json, &report, error, error_size);
    free(report);
    return err;
}

esp_err_t app_mqtt_register_report(app_mqtt_t *s, const char *id, const char *json,
                                   char *error, size_t error_size)
{
    if (!s) return ESP_ERR_INVALID_STATE;
    mqtt_report_t *r = NULL;
    esp_err_t err = mqtt_report_parse(id, s->device_id, json, &r, error, error_size);
    if (err != ESP_OK) return err;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    size_t count = 0;
    for (mqtt_report_t *current = s->reports; current; current = current->next) {
        if (!strcmp(current->id, id)) { err = ESP_ERR_INVALID_STATE; break; }
        ++count;
    }
    if (count >= APP_MQTT_MAX_REPORTS) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) { r->next = s->reports; s->reports = r; }
    xSemaphoreGive(s->lock);
    if (err != ESP_OK) {
        free(r);
        if (error && error_size) snprintf(error, error_size, "duplicate report or report limit reached");
    }
    return err;
}

bool app_mqtt_unregister_report(app_mqtt_t *s, const char *id)
{
    if (!s || !id) return false;
    bool removed = false;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    mqtt_report_t **link = &s->reports;
    while (*link) {
        if (!strcmp((*link)->id, id)) {
            mqtt_report_t *r = *link;
            *link = r->next; free(r); removed = true; break;
        }
        link = &(*link)->next;
    }
    xSemaphoreGive(s->lock);
    return removed;
}

void app_mqtt_clear_reports(app_mqtt_t *s)
{
    if (!s) return;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    while (s->reports) { mqtt_report_t *r = s->reports; s->reports = r->next; free(r); }
    xSemaphoreGive(s->lock);
}

esp_err_t app_mqtt_start(app_registers_t *registers, app_http_server_t *http, app_mqtt_t **out)
{
    if (!registers || !http || !out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    app_mqtt_t *s = calloc(1, sizeof(*s));
    if (!s) return ESP_ERR_NO_MEM;
    s->registers = registers;
    s->lock = xSemaphoreCreateMutex();
    esp_err_t err = s->lock ? ESP_OK : ESP_ERR_NO_MEM;
    uint8_t mac[6];
    if (err == ESP_OK) err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err == ESP_OK) snprintf(s->device_id, sizeof(s->device_id), "%02X%02X%02X%02X%02X%02X",
                               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (err == ESP_OK) err = mqtt_config_load(&s->broker.config);
    if (err != ESP_OK) goto fail;
    mqtt_broker_t *broker = &s->broker;
    if (broker->config.enabled) {
        if (!mqtt_config_valid(&broker->config)) {
            atomic_store(&broker->state, 4);
            ESP_LOGW(TAG, "MQTT has invalid configuration");
        } else {
            char client_id[32];
            snprintf(client_id, sizeof(client_id), "jsp32-%s", s->device_id);
            esp_mqtt_client_config_t config = {
                .broker.address.uri = broker->config.url,
                .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
                .credentials.client_id = client_id,
                .credentials.username = broker->config.username[0] ? broker->config.username : NULL,
                .credentials.authentication.password = broker->config.password[0] ? broker->config.password : NULL,
                .network.reconnect_timeout_ms = 5000,
                .network.timeout_ms = 10000,
                .outbox.limit = 32768,
            };
            broker->client = esp_mqtt_client_init(&config);
            if (!broker->client) atomic_store(&broker->state, 4);
            else {
                err = esp_mqtt_client_register_event(broker->client, ESP_EVENT_ANY_ID, broker_event, broker);
                atomic_store(&broker->state, 1);
                if (err == ESP_OK) err = esp_mqtt_client_start(broker->client);
                if (err != ESP_OK) { atomic_store(&broker->state, 4); ESP_LOGW(TAG, "MQTT failed to start"); }
            }
        }
    }
    err = app_registers_subscribe(registers, registers_changed, s, &s->subscription);
    if (err != ESP_OK) goto fail;
    err = mqtt_http_register(s, http);
    if (err != ESP_OK) goto fail;
    if (xTaskCreate(report_task, "mqtt_reports", 8192, s, 4, NULL) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        app_http_server_unregister_owner(http, "mqtt");
        goto fail;
    }
    *out = s;
    return ESP_OK;
fail:
    if (s->subscription) app_registers_unsubscribe(registers, s->subscription);
    if (s->broker.client) esp_mqtt_client_destroy(s->broker.client);
    if (s->lock) vSemaphoreDelete(s->lock);
    free(s);
    return err;
}
