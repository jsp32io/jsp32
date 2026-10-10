#include "mqtt_report.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"

static bool identifier(const char *s, size_t max, bool plugin)
{
    if (!s || !*s || strlen(s) > max ||
        (plugin ? (*s < 'a' || *s > 'z') : !isalpha((unsigned char)*s))) return false;
    for (; *s; ++s) {
        if (!(plugin ? (*s >= 'a' && *s <= 'z') : isalnum((unsigned char)*s)) &&
            !isdigit((unsigned char)*s) && *s != '_' && !(plugin && *s == '-')) return false;
    }
    return true;
}

static bool keys(const cJSON *object, const char *const *allowed)
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

static bool integer(const cJSON *root, const char *key, uint32_t fallback,
                    uint32_t min, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) { *out = fallback; return true; }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < min || item->valuedouble > max) return false;
    *out = (uint32_t)item->valuedouble;
    return true;
}

static bool number(const cJSON *root, const char *key, double fallback, double *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item) { *out = fallback; return true; }
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble)) return false;
    *out = item->valuedouble;
    return true;
}

esp_err_t mqtt_report_parse(const char *id, const char *device_id, const char *json,
                             mqtt_report_t **out, char *error, size_t error_size)
{
    *out = NULL;
    const char *reason = "invalid telemetry declaration";
    cJSON *root = json ? cJSON_ParseWithOpts(json, NULL, true) : NULL;
    mqtt_report_t *r = calloc(1, sizeof(*r));
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (!r) { result = ESP_ERR_NO_MEM; reason = "out of memory"; goto fail; }
    static const char *const top_keys[] = {"topic", "mode", "intervalMs", "minIntervalMs",
        "heartbeatMs", "qos", "retain", "fields", NULL};
    if (!identifier(id, 32, true) || !device_id || !keys(root, top_keys)) goto fail;
    strcpy(r->id, id);
    const cJSON *topic = cJSON_GetObjectItemCaseSensitive(root, "topic");
    if (topic) {
        reason = "topic must be a nonempty string without + or # (max 256 bytes)";
        if (!cJSON_IsString(topic) || !*topic->valuestring || strlen(topic->valuestring) > 256 ||
            strpbrk(topic->valuestring, "+#")) goto fail;
        strcpy(r->topic, topic->valuestring);
    } else snprintf(r->topic, sizeof(r->topic), "/jsp32/%s/%s", device_id, id);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    reason = "mode must be interval or change";
    if (mode && (!cJSON_IsString(mode) ||
        (strcmp(mode->valuestring, "interval") && strcmp(mode->valuestring, "change")))) goto fail;
    r->change_mode = mode && !strcmp(mode->valuestring, "change");
    reason = "invalid telemetry timing or QoS";
    uint32_t qos;
    if (!integer(root, "intervalMs", 1000, 10, 86400000, &r->interval_ms) ||
        !integer(root, "minIntervalMs", 200, 10, 86400000, &r->min_interval_ms) ||
        !integer(root, "heartbeatMs", 10000, 10, 86400000, &r->heartbeat_ms) ||
        !integer(root, "qos", 0, 0, 2, &qos)) goto fail;
    if ((r->change_mode && cJSON_HasObjectItem(root, "intervalMs")) ||
        (!r->change_mode && (cJSON_HasObjectItem(root, "minIntervalMs") ||
                            cJSON_HasObjectItem(root, "heartbeatMs"))) ||
        (r->change_mode && r->heartbeat_ms < r->min_interval_ms)) goto fail;
    r->qos = (int)qos;
    const cJSON *retain = cJSON_GetObjectItemCaseSensitive(root, "retain");
    if (retain && !cJSON_IsBool(retain)) goto fail;
    r->retain = cJSON_IsTrue(retain);
    const cJSON *fields = cJSON_GetObjectItemCaseSensitive(root, "fields");
    reason = "fields must contain 0 to 64 named register definitions";
    if (!cJSON_IsObject(fields)) goto fail;
    static const char *const field_keys[] = {"name", "address", "type", "wordOrder", "scale", "offset", "trigger", NULL};
    const char *const types[] = {"boolean", "uint16", "int16", "uint32", "int32", "float32"};
    size_t words = 0;
    const cJSON *f;
    cJSON_ArrayForEach(f, fields) {
        reason = "fields must contain 0 to 64 named register definitions";
        if (r->field_count == APP_MQTT_MAX_FIELDS || !identifier(f->string, 63, false) ||
            !keys(f, field_keys)) goto fail;
        mqtt_field_t *dest = &r->fields[r->field_count];
        for (size_t i = 0; i < r->field_count; ++i)
            if (!strcmp(r->fields[i].name, f->string)) goto fail;
        strcpy(dest->name, f->string);
        const cJSON *trigger = cJSON_GetObjectItemCaseSensitive(f, "trigger");
        reason = "field trigger must be a boolean";
        if (trigger && !cJSON_IsBool(trigger)) goto fail;
        dest->trigger = !trigger || cJSON_IsTrue(trigger);
        const cJSON *display_name = cJSON_GetObjectItemCaseSensitive(f, "name");
        reason = "field name must be a nonempty display string (max 128 bytes)";
        if (display_name && (!cJSON_IsString(display_name) || !*display_name->valuestring ||
                             strlen(display_name->valuestring) > 128)) goto fail;
        const cJSON *address = cJSON_GetObjectItemCaseSensitive(f, "address");
        const cJSON *type = cJSON_GetObjectItemCaseSensitive(f, "type");
        reason = "invalid field address, type, or register range";
        if (!cJSON_IsString(address) || !cJSON_IsString(type) ||
            app_register_address_parse(address->valuestring, &dest->address) != ESP_OK) goto fail;
        unsigned t;
        for (t = 0; t < sizeof(types)/sizeof(types[0]); ++t)
            if (!strcmp(type->valuestring, types[t])) break;
        if (t == sizeof(types)/sizeof(types[0])) goto fail;
        dest->type = (mqtt_value_type_t)t;
        bool bit = app_register_area_is_bit(dest->address.area);
        if ((dest->type == MQTT_BOOLEAN) != bit) goto fail;
        unsigned width = t >= MQTT_U32 ? 2 : 1;
        if ((size_t)dest->address.index + width > app_registers_area_count(dest->address.area)) goto fail;
        words += width;
        if (words > CONFIG_APP_REGISTERS_MAX_TRANSACTION) goto fail;
        const cJSON *order = cJSON_GetObjectItemCaseSensitive(f, "wordOrder");
        reason = "wordOrder must be highFirst or lowFirst for two-word fields";
        if (order && (width != 2 || !cJSON_IsString(order) ||
            (strcmp(order->valuestring, "highFirst") && strcmp(order->valuestring, "lowFirst")))) goto fail;
        dest->low_first = order && !strcmp(order->valuestring, "lowFirst");
        reason = "scale and offset must be finite numbers on numeric fields";
        if (bit && (cJSON_HasObjectItem(f, "scale") || cJSON_HasObjectItem(f, "offset"))) goto fail;
        if (!number(f, "scale", 1, &dest->scale) || !number(f, "offset", 0, &dest->offset)) goto fail;
        ++r->field_count;
    }
    r->pending = true;
    *out = r;
    cJSON_Delete(root);
    return ESP_OK;
fail:
    if (error && error_size) snprintf(error, error_size, "%s", reason);
    free(r);
    cJSON_Delete(root);
    return result;
}

bool mqtt_report_matches(const mqtt_report_t *r, app_register_address_t address)
{
    for (size_t i = 0; i < r->field_count; ++i) {
        const mqtt_field_t *f = &r->fields[i];
        if (!f->trigger) continue;
        unsigned width = f->type >= MQTT_U32 ? 2 : 1;
        if (address.area == f->address.area && address.index >= f->address.index &&
            address.index < (unsigned)f->address.index + width) return true;
    }
    return false;
}

char *mqtt_report_encode(const mqtt_report_t *r, app_registers_t *registers, uint64_t now)
{
    app_register_address_t addresses[APP_MQTT_MAX_FIELDS * 2];
    uint16_t values[APP_MQTT_MAX_FIELDS * 2];
    size_t count = 0;
    for (size_t i = 0; i < r->field_count; ++i) {
        addresses[count++] = r->fields[i].address;
        if (r->fields[i].type >= MQTT_U32) {
            addresses[count] = r->fields[i].address;
            addresses[count++].index++;
        }
    }
    if (app_registers_read_many(registers, addresses, values, count, NULL) != ESP_OK) return NULL;
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON *data = cJSON_AddObjectToObject(root, "data");
    if (!data || !cJSON_AddNumberToObject(root, "uptimeMs", (double)now)) goto fail;
    size_t cursor = 0;
    for (size_t i = 0; i < r->field_count; ++i) {
        const mqtt_field_t *f = &r->fields[i];
        uint16_t word = values[cursor++];
        double value = word;
        if (f->type == MQTT_BOOLEAN) {
            if (!cJSON_AddBoolToObject(data, f->name, word != 0)) goto fail;
            continue;
        }
        if (f->type == MQTT_I16) value = word >= 0x8000 ? (double)word - 65536 : word;
        if (f->type >= MQTT_U32) {
            uint16_t second = values[cursor++];
            uint32_t bits = f->low_first ? ((uint32_t)second << 16) | word : ((uint32_t)word << 16) | second;
            if (f->type == MQTT_U32) value = bits;
            else if (f->type == MQTT_I32) value = bits >= 0x80000000u ? (double)bits - 4294967296.0 : bits;
            else { float number; memcpy(&number, &bits, sizeof(number)); value = number; }
        }
        value = value * f->scale + f->offset;
        if (!(isfinite(value) ? cJSON_AddNumberToObject(data, f->name, value) :
                              cJSON_AddNullToObject(data, f->name))) goto fail;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json && strlen(json) > APP_MQTT_MAX_PAYLOAD) { cJSON_free(json); return NULL; }
    return json;
fail:
    cJSON_Delete(root);
    return NULL;
}

bool mqtt_report_due(mqtt_report_t *r, uint64_t now, bool connected)
{
    if (r->field_count == 0) return false;
    if (!r->change_mode && now >= r->next_interval_ms) {
        r->pending = true;
        r->next_interval_ms = now + r->interval_ms;
    }
    if (!connected || now < r->retry_after_ms) return false;
    if (r->change_mode && now - r->last_ms >= r->heartbeat_ms) r->pending = true;
    return r->pending && (!r->last_ms || !r->change_mode || now - r->last_ms >= r->min_interval_ms);
}

void mqtt_report_sent(mqtt_report_t *r, uint64_t now)
{
    r->pending = false;
    r->last_ms = now;
}
