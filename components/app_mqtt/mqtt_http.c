#include "mqtt_internal.h"
#include <stdlib.h>
#include <string.h>

static esp_err_t send_json(httpd_req_t *req, cJSON *root)
{
    char *body = root ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static esp_err_t get_config(httpd_req_t *req)
{
    mqtt_broker_config_t config;
    if (mqtt_config_load(&config) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot read MQTT config");
    cJSON *root = cJSON_CreateObject();
    if (root) {
        cJSON_AddBoolToObject(root, "enabled", config.enabled);
        cJSON_AddStringToObject(root, "url", config.url);
        cJSON_AddStringToObject(root, "username", config.username);
        cJSON_AddBoolToObject(root, "passwordSet", config.password[0] != 0);
    }
    memset(&config, 0, sizeof(config));
    return send_json(req, root);
}

static bool copy_string(const cJSON *item, char *out, size_t capacity)
{
    if (!cJSON_IsString(item) || strlen(item->valuestring) >= capacity) return false;
    strcpy(out, item->valuestring);
    return true;
}

static bool valid_keys(const cJSON *object, const char *const *allowed)
{
    if (!cJSON_IsObject(object)) return false;
    const cJSON *item;
    cJSON_ArrayForEach(item, object) {
        bool found = false;
        for (size_t i = 0; allowed[i]; ++i) if (!strcmp(item->string, allowed[i])) found = true;
        if (!found) return false;
        for (const cJSON *prior = object->child; prior != item; prior = prior->next)
            if (!strcmp(prior->string, item->string)) return false;
    }
    return true;
}

static esp_err_t put_config(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 4096)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid config size");
    char *body = calloc(1, req->content_len + 1);
    if (!body) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) { free(body); return ESP_FAIL; }
        received += count;
    }
    cJSON *root = cJSON_ParseWithOpts(body, NULL, true);
    memset(body, 0, req->content_len); free(body);
    mqtt_broker_config_t config;
    esp_err_t err = mqtt_config_load(&config);
    static const char *const root_keys[] = {"enabled", "url", "username", "password", NULL};
    bool valid = err == ESP_OK && valid_keys(root, root_keys);
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(root, "enabled");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
    valid = valid && cJSON_IsBool(enabled) &&
        copy_string(cJSON_GetObjectItemCaseSensitive(root, "url"), config.url, sizeof(config.url)) &&
        copy_string(cJSON_GetObjectItemCaseSensitive(root, "username"), config.username, sizeof(config.username)) &&
        (!password || copy_string(password, config.password, sizeof(config.password)));
    config.enabled = cJSON_IsTrue(enabled);
    valid = valid && mqtt_config_valid(&config);
    if (valid) err = mqtt_config_save(&config);
    memset(&config, 0, sizeof(config));
    cJSON_Delete(root);
    if (!valid) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected enabled, url, username and optional password; valid mqtt:// or mqtts:// URL; no URL credentials");
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot save MQTT config");
    root = cJSON_CreateObject();
    if (root) cJSON_AddBoolToObject(root, "restartRequired", true);
    return send_json(req, root);
}

static esp_err_t get_status(httpd_req_t *req)
{
    app_mqtt_t *s = req->user_ctx;
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_json(req, NULL);
    cJSON_AddStringToObject(root, "deviceId", s->device_id);
    const char *states[] = {"disabled", "connecting", "connected", "disconnected", "error"};
    cJSON_AddStringToObject(root, "state", states[atomic_load(&s->broker.state)]);
    cJSON_AddNumberToObject(root, "queued", atomic_load(&s->broker.queued));
    cJSON_AddNumberToObject(root, "rejected", atomic_load(&s->broker.rejected));
    cJSON *reports = cJSON_AddArrayToObject(root, "reports");
    xSemaphoreTake(s->lock, portMAX_DELAY);
    for (mqtt_report_t *r = s->reports; r && reports; r = r->next) {
        cJSON *item = cJSON_CreateObject();
        if (!item || !cJSON_AddItemToArray(reports, item)) { cJSON_Delete(item); break; }
        cJSON_AddStringToObject(item, "pluginId", r->id);
        cJSON_AddStringToObject(item, "topic", r->topic);
        cJSON_AddStringToObject(item, "mode", r->change_mode ? "change" : "interval");
        cJSON_AddNumberToObject(item, "fieldCount", r->field_count);
    }
    xSemaphoreGive(s->lock);
    return send_json(req, root);
}

esp_err_t mqtt_http_register(app_mqtt_t *s, app_http_server_t *http)
{
    const httpd_uri_t handlers[] = {
        {.uri = "/api/v1/mqtt/config", .method = HTTP_GET, .handler = get_config, .user_ctx = s},
        {.uri = "/api/v1/mqtt/config", .method = HTTP_PUT, .handler = put_config, .user_ctx = s},
        {.uri = "/api/v1/mqtt/status", .method = HTTP_GET, .handler = get_status, .user_ctx = s},
    };
    return app_http_server_register(http, "mqtt", handlers, sizeof(handlers)/sizeof(handlers[0]));
}
