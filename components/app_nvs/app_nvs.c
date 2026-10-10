#include "app_nvs.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#define NVS_PARTITION "nvs"
#define MANAGED_NAMESPACE "jsp32_config"
#define WIFI_PASSWORD_KEY "wifi_pass"
#define MAX_NAMESPACES 254

struct app_nvs {
    SemaphoreHandle_t mutex;
};

static app_nvs_t s_store;

static bool name_is_valid(const char *name, size_t maximum)
{
    if (!name || !name[0] || strnlen(name, maximum) >= maximum) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
        if (!(isalnum(*p) || *p == '_' || *p == '-' || *p == '.')) {
            return false;
        }
    }
    return true;
}

static bool prefix_is_valid(const char *prefix, size_t maximum)
{
    if (!prefix || !prefix[0]) {
        return true;
    }
    return name_is_valid(prefix, maximum);
}

app_nvs_policy_t app_nvs_policy(const char *namespace_name)
{
    if (!namespace_name) {
        return APP_NVS_POLICY_HIDDEN;
    }
    if (strcmp(namespace_name, MANAGED_NAMESPACE) == 0) {
        return APP_NVS_POLICY_MANAGED;
    }
    if (strcmp(namespace_name, "user") == 0 ||
        strncmp(namespace_name, "user_", 5) == 0) {
        return APP_NVS_POLICY_USER;
    }
    return APP_NVS_POLICY_HIDDEN;
}

static bool entry_is_sensitive(const char *namespace_name, const char *key)
{
    return strcmp(namespace_name, MANAGED_NAMESPACE) == 0 &&
           (strcmp(key, WIFI_PASSWORD_KEY) == 0 ||
            strcmp(key, "ap_pass") == 0 ||
            strcmp(key, "mqtt_password") == 0);
}

const char *app_nvs_type_name(nvs_type_t type)
{
    switch (type) {
    case NVS_TYPE_I8: return "i8";
    case NVS_TYPE_U8: return "u8";
    case NVS_TYPE_I16: return "i16";
    case NVS_TYPE_U16: return "u16";
    case NVS_TYPE_I32: return "i32";
    case NVS_TYPE_U32: return "u32";
    case NVS_TYPE_I64: return "i64";
    case NVS_TYPE_U64: return "u64";
    case NVS_TYPE_FLOAT: return "float";
    case NVS_TYPE_DOUBLE: return "double";
    case NVS_TYPE_STR: return "string";
    case NVS_TYPE_BLOB: return "blob";
    default: return NULL;
    }
}

static size_t fixed_type_size(nvs_type_t type)
{
    switch (type) {
    case NVS_TYPE_I8:
    case NVS_TYPE_U8: return 1;
    case NVS_TYPE_I16:
    case NVS_TYPE_U16: return 2;
    case NVS_TYPE_I32:
    case NVS_TYPE_U32:
    case NVS_TYPE_FLOAT: return 4;
    case NVS_TYPE_I64:
    case NVS_TYPE_U64:
    case NVS_TYPE_DOUBLE: return 8;
    default: return 0;
    }
}

static esp_err_t find_type_locked(const char *namespace_name, const char *key,
                                  nvs_type_t *out_type)
{
    nvs_iterator_t iterator = NULL;
    esp_err_t err = nvs_entry_find(NVS_PARTITION, namespace_name,
                                   NVS_TYPE_ANY, &iterator);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        err = nvs_entry_info(iterator, &info);
        if (err != ESP_OK) {
            break;
        }
        if (strcmp(info.key, key) == 0) {
            *out_type = info.type;
            nvs_release_iterator(iterator);
            return ESP_OK;
        }
        err = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t read_value_locked(nvs_handle_t handle, const char *key,
                                   nvs_type_t type,
                                   app_nvs_value_t *out_value)
{
    memset(out_value, 0, sizeof(*out_value));
    out_value->type = type;
    esp_err_t err;
    switch (type) {
    case NVS_TYPE_I8: err = nvs_get_i8(handle, key, &out_value->data.i8); break;
    case NVS_TYPE_U8: err = nvs_get_u8(handle, key, &out_value->data.u8); break;
    case NVS_TYPE_I16: err = nvs_get_i16(handle, key, &out_value->data.i16); break;
    case NVS_TYPE_U16: err = nvs_get_u16(handle, key, &out_value->data.u16); break;
    case NVS_TYPE_I32: err = nvs_get_i32(handle, key, &out_value->data.i32); break;
    case NVS_TYPE_U32: err = nvs_get_u32(handle, key, &out_value->data.u32); break;
    case NVS_TYPE_I64: err = nvs_get_i64(handle, key, &out_value->data.i64); break;
    case NVS_TYPE_U64: err = nvs_get_u64(handle, key, &out_value->data.u64); break;
    case NVS_TYPE_FLOAT: err = nvs_get_float(handle, key, &out_value->data.f32); break;
    case NVS_TYPE_DOUBLE: err = nvs_get_double(handle, key, &out_value->data.f64); break;
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: {
        size_t size = 0;
        err = type == NVS_TYPE_STR ? nvs_get_str(handle, key, NULL, &size)
                                   : nvs_get_blob(handle, key, NULL, &size);
        if (err != ESP_OK) {
            break;
        }
        void *data = malloc(size == 0 ? 1 : size);
        if (!data) {
            return ESP_ERR_NO_MEM;
        }
        err = type == NVS_TYPE_STR ? nvs_get_str(handle, key, data, &size)
                                   : nvs_get_blob(handle, key, data, &size);
        if (err != ESP_OK) {
            free(data);
            break;
        }
        out_value->data.bytes.data = data;
        out_value->data.bytes.size = type == NVS_TYPE_STR && size > 0
                                         ? size - 1 : size;
        break;
    }
    default:
        err = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    return err;
}

void app_nvs_value_free(app_nvs_value_t *value)
{
    if (!value) {
        return;
    }
    if (value->type == NVS_TYPE_STR || value->type == NVS_TYPE_BLOB) {
        free(value->data.bytes.data);
    }
    memset(value, 0, sizeof(*value));
}

static const void *value_bytes(const app_nvs_value_t *value, size_t *out_size)
{
    size_t fixed = fixed_type_size(value->type);
    if (fixed > 0) {
        *out_size = fixed;
        return &value->data;
    }
    *out_size = value->data.bytes.size;
    return value->data.bytes.data;
}

static esp_err_t value_etag(const app_nvs_value_t *value,
                            char output[APP_NVS_ETAG_HEX_SIZE])
{
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    uint8_t type = (uint8_t)value->type;
    uint8_t length[8];
    size_t size = 0;
    const void *data = value_bytes(value, &size);
    for (size_t i = 0; i < sizeof(length); ++i) {
        length[sizeof(length) - 1 - i] =
            (uint8_t)((uint64_t)size >> (i * 8));
    }
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS ||
        psa_hash_update(&hash, &type, 1) != PSA_SUCCESS ||
        psa_hash_update(&hash, length, sizeof(length)) != PSA_SUCCESS ||
        (size > 0 && psa_hash_update(&hash, data, size) != PSA_SUCCESS)) {
        psa_hash_abort(&hash);
        return ESP_FAIL;
    }
    uint8_t digest[32];
    size_t digest_size = 0;
    if (psa_hash_finish(&hash, digest, sizeof(digest), &digest_size) !=
            PSA_SUCCESS || digest_size != sizeof(digest)) {
        psa_hash_abort(&hash);
        return ESP_FAIL;
    }
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(digest); ++i) {
        output[i * 2] = digits[digest[i] >> 4];
        output[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    output[64] = '\0';
    return ESP_OK;
}

esp_err_t app_nvs_init(app_nvs_t **out_store)
{
    if (!out_store) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_store.mutex) {
        s_store.mutex = xSemaphoreCreateMutex();
        if (!s_store.mutex) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        return ESP_FAIL;
    }
    *out_store = &s_store;
    return ESP_OK;
}

esp_err_t app_nvs_stats(app_nvs_t *store, nvs_stats_t *out_stats)
{
    if (store != &s_store || !store->mutex || !out_stats) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    esp_err_t err = nvs_get_stats(NVS_PARTITION, out_stats);
    xSemaphoreGive(store->mutex);
    return err;
}

static int compare_namespaces(const void *left, const void *right)
{
    const app_nvs_namespace_info_t *a = left;
    const app_nvs_namespace_info_t *b = right;
    return strcmp(a->name, b->name);
}

static bool namespace_seen(app_nvs_namespace_info_t *entries, size_t count,
                           const char *name)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t app_nvs_list_namespaces(app_nvs_t *store, const char *prefix,
                                  const char *after,
                                  app_nvs_namespace_info_t *entries,
                                  size_t capacity, size_t *out_count,
                                  bool *out_has_more)
{
    if (store != &s_store || !store->mutex || !entries || capacity == 0 ||
        !out_count || !out_has_more ||
        !prefix_is_valid(prefix, NVS_NS_NAME_MAX_SIZE) ||
        (after && after[0] && !name_is_valid(after, NVS_NS_NAME_MAX_SIZE))) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_namespace_info_t *all = calloc(MAX_NAMESPACES, sizeof(*all));
    if (!all) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    size_t all_count = 0;
    nvs_iterator_t iterator = NULL;
    esp_err_t err = nvs_entry_find(NVS_PARTITION, NULL, NVS_TYPE_ANY,
                                   &iterator);
    while (err == ESP_OK) {
        nvs_entry_info_t item;
        err = nvs_entry_info(iterator, &item);
        if (err != ESP_OK) {
            break;
        }
        app_nvs_policy_t policy = app_nvs_policy(item.namespace_name);
        if (policy != APP_NVS_POLICY_HIDDEN &&
            !namespace_seen(all, all_count, item.namespace_name) &&
            all_count < MAX_NAMESPACES) {
            strlcpy(all[all_count].name, item.namespace_name,
                    sizeof(all[all_count].name));
            all[all_count].policy = policy;
            nvs_handle_t handle;
            if (nvs_open(item.namespace_name, NVS_READONLY, &handle) == ESP_OK) {
                nvs_get_used_entry_count(handle, &all[all_count].used_entries);
                nvs_close(handle);
            }
            ++all_count;
        }
        err = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    xSemaphoreGive(store->mutex);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        free(all);
        return err;
    }
    qsort(all, all_count, sizeof(*all), compare_namespaces);
    size_t count = 0;
    size_t eligible = 0;
    for (size_t i = 0; i < all_count; ++i) {
        if ((prefix && prefix[0] &&
             strncmp(all[i].name, prefix, strlen(prefix)) != 0) ||
            (after && after[0] && strcmp(all[i].name, after) <= 0)) {
            continue;
        }
        if (count < capacity) {
            entries[count++] = all[i];
        }
        ++eligible;
    }
    free(all);
    *out_count = count;
    *out_has_more = eligible > capacity;
    return ESP_OK;
}

esp_err_t app_nvs_namespace_info(app_nvs_t *store, const char *namespace_name,
                                 app_nvs_namespace_info_t *out_info)
{
    if (store != &s_store || !store->mutex ||
        !name_is_valid(namespace_name, NVS_NS_NAME_MAX_SIZE) || !out_info) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_policy_t policy = app_nvs_policy(namespace_name);
    if (policy == APP_NVS_POLICY_HIDDEN) {
        return APP_NVS_ERR_HIDDEN;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t used = 0;
        err = nvs_get_used_entry_count(handle, &used);
        nvs_close(handle);
        if (err == ESP_OK) {
            memset(out_info, 0, sizeof(*out_info));
            strlcpy(out_info->name, namespace_name, sizeof(out_info->name));
            out_info->policy = policy;
            out_info->used_entries = used;
        }
    }
    xSemaphoreGive(store->mutex);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static size_t entry_size_locked(nvs_handle_t handle, const char *key,
                                nvs_type_t type)
{
    size_t size = fixed_type_size(type);
    if (type == NVS_TYPE_STR) {
        return nvs_get_str(handle, key, NULL, &size) == ESP_OK && size > 0
                   ? size - 1 : 0;
    }
    if (type == NVS_TYPE_BLOB) {
        return nvs_get_blob(handle, key, NULL, &size) == ESP_OK ? size : 0;
    }
    return size;
}

static void insert_entry(app_nvs_entry_info_t *entries, size_t count,
                         app_nvs_entry_info_t item)
{
    size_t position = count;
    while (position > 0 && strcmp(entries[position - 1].key, item.key) > 0) {
        entries[position] = entries[position - 1];
        --position;
    }
    entries[position] = item;
}

static int compare_entries(const void *left, const void *right)
{
    const app_nvs_entry_info_t *a = left;
    const app_nvs_entry_info_t *b = right;
    return strcmp(a->key, b->key);
}

esp_err_t app_nvs_list_entries(app_nvs_t *store, const char *namespace_name,
                               const char *prefix, const char *after,
                               app_nvs_entry_info_t *entries,
                               size_t capacity, size_t *out_count,
                               bool *out_has_more)
{
    if (store != &s_store || !store->mutex ||
        !name_is_valid(namespace_name, NVS_NS_NAME_MAX_SIZE) || !entries ||
        capacity == 0 || !out_count || !out_has_more ||
        !prefix_is_valid(prefix, NVS_KEY_NAME_MAX_SIZE) ||
        (after && after[0] && !name_is_valid(after, NVS_KEY_NAME_MAX_SIZE))) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_policy_t policy = app_nvs_policy(namespace_name);
    if (policy == APP_NVS_POLICY_HIDDEN) {
        return APP_NVS_ERR_HIDDEN;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(store->mutex);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
    }
    size_t count = 0;
    size_t eligible = 0;
    nvs_iterator_t iterator = NULL;
    err = nvs_entry_find_in_handle(handle, NVS_TYPE_ANY, &iterator);
    while (err == ESP_OK) {
        nvs_entry_info_t source;
        err = nvs_entry_info(iterator, &source);
        if (err != ESP_OK) {
            break;
        }
        if ((!prefix || !prefix[0] ||
             strncmp(source.key, prefix, strlen(prefix)) == 0) &&
            (!after || !after[0] || strcmp(source.key, after) > 0) &&
            app_nvs_type_name(source.type)) {
            app_nvs_entry_info_t item = {
                .type = source.type,
                .sensitive = entry_is_sensitive(namespace_name, source.key),
                .writable = policy != APP_NVS_POLICY_HIDDEN,
            };
            item.readable = !item.sensitive;
            strlcpy(item.key, source.key, sizeof(item.key));
            item.size = item.sensitive ? 0
                                       : entry_size_locked(handle, source.key,
                                                           source.type);
            ++eligible;
            if (count < capacity) {
                insert_entry(entries, count++, item);
            } else if (strcmp(item.key, entries[count - 1].key) < 0) {
                entries[count - 1] = item;
                qsort(entries, count, sizeof(*entries), compare_entries);
            }
        }
        err = nvs_entry_next(&iterator);
    }
    nvs_release_iterator(iterator);
    nvs_close(handle);
    xSemaphoreGive(store->mutex);
    if (err != ESP_ERR_NVS_NOT_FOUND) {
        return err;
    }
    *out_count = count;
    *out_has_more = eligible > capacity;
    return ESP_OK;
}

static esp_err_t get_from_handle_locked(
    nvs_handle_t handle, const char *namespace_name, const char *key,
    app_nvs_value_t *out_value, char out_etag[APP_NVS_ETAG_HEX_SIZE])
{
    nvs_type_t type;
    esp_err_t err = find_type_locked(namespace_name, key, &type);
    if (err != ESP_OK) {
        return err;
    }
    err = read_value_locked(handle, key, type, out_value);
    if (err == ESP_OK) {
        err = value_etag(out_value, out_etag);
    }
    if (err != ESP_OK) {
        app_nvs_value_free(out_value);
    }
    return err;
}

esp_err_t app_nvs_get(app_nvs_t *store, const char *namespace_name,
                      const char *key, app_nvs_value_t *out_value,
                      char out_etag[APP_NVS_ETAG_HEX_SIZE])
{
    if (store != &s_store || !store->mutex ||
        !name_is_valid(namespace_name, NVS_NS_NAME_MAX_SIZE) ||
        !name_is_valid(key, NVS_KEY_NAME_MAX_SIZE) || !out_value ||
        !out_etag) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_policy_t policy = app_nvs_policy(namespace_name);
    if (policy == APP_NVS_POLICY_HIDDEN) {
        return APP_NVS_ERR_HIDDEN;
    }
    if (entry_is_sensitive(namespace_name, key)) {
        return APP_NVS_ERR_MANAGED;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = get_from_handle_locked(handle, namespace_name, key, out_value,
                                     out_etag);
        nvs_close(handle);
    }
    xSemaphoreGive(store->mutex);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

static esp_err_t set_value_locked(nvs_handle_t handle, const char *key,
                                  const app_nvs_value_t *value)
{
    switch (value->type) {
    case NVS_TYPE_I8: return nvs_set_i8(handle, key, value->data.i8);
    case NVS_TYPE_U8: return nvs_set_u8(handle, key, value->data.u8);
    case NVS_TYPE_I16: return nvs_set_i16(handle, key, value->data.i16);
    case NVS_TYPE_U16: return nvs_set_u16(handle, key, value->data.u16);
    case NVS_TYPE_I32: return nvs_set_i32(handle, key, value->data.i32);
    case NVS_TYPE_U32: return nvs_set_u32(handle, key, value->data.u32);
    case NVS_TYPE_I64: return nvs_set_i64(handle, key, value->data.i64);
    case NVS_TYPE_U64: return nvs_set_u64(handle, key, value->data.u64);
    case NVS_TYPE_FLOAT: return nvs_set_float(handle, key, value->data.f32);
    case NVS_TYPE_DOUBLE: return nvs_set_double(handle, key, value->data.f64);
    case NVS_TYPE_STR:
        return value->data.bytes.data
                   ? nvs_set_str(handle, key, value->data.bytes.data)
                   : ESP_ERR_INVALID_ARG;
    case NVS_TYPE_BLOB:
        return value->data.bytes.data || value->data.bytes.size == 0
                   ? nvs_set_blob(handle, key, value->data.bytes.data,
                                  value->data.bytes.size)
                   : ESP_ERR_INVALID_ARG;
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

static esp_err_t check_condition_locked(
    nvs_handle_t handle, const char *namespace_name, const char *key,
    app_nvs_condition_t condition, const char *expected_etag,
    bool *out_exists, nvs_type_t *out_type)
{
    nvs_type_t existing_type = NVS_TYPE_ANY;
    esp_err_t found = find_type_locked(namespace_name, key, &existing_type);
    bool exists = found == ESP_OK;
    if (found != ESP_OK && found != ESP_ERR_NOT_FOUND) {
        return found;
    }
    if ((condition == APP_NVS_CONDITION_CREATE_ONLY && exists) ||
        (condition == APP_NVS_CONDITION_MATCH && !exists)) {
        return APP_NVS_ERR_PRECONDITION;
    }
    if (condition == APP_NVS_CONDITION_MATCH) {
        if (!expected_etag || strlen(expected_etag) != 64) {
            return ESP_ERR_INVALID_ARG;
        }
        app_nvs_value_t current;
        char actual[APP_NVS_ETAG_HEX_SIZE];
        esp_err_t err = read_value_locked(handle, key, existing_type, &current);
        if (err != ESP_OK) {
            return err;
        }
        err = value_etag(&current, actual);
        app_nvs_value_free(&current);
        if (err != ESP_OK) {
            return err;
        }
        if (strcasecmp(actual, expected_etag) != 0) {
            return APP_NVS_ERR_PRECONDITION;
        }
    }
    *out_exists = exists;
    if (out_type) {
        *out_type = existing_type;
    }
    return ESP_OK;
}

static esp_err_t validate_write(const char *namespace_name, const char *key,
                                const app_nvs_value_t *value,
                                app_nvs_condition_t condition)
{
    if (!name_is_valid(namespace_name, NVS_NS_NAME_MAX_SIZE) ||
        !name_is_valid(key, NVS_KEY_NAME_MAX_SIZE) ||
        condition < APP_NVS_CONDITION_UPSERT ||
        condition > APP_NVS_CONDITION_MATCH ||
        (value && !app_nvs_type_name(value->type))) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_policy_t policy = app_nvs_policy(namespace_name);
    return policy == APP_NVS_POLICY_HIDDEN ? APP_NVS_ERR_HIDDEN : ESP_OK;
}

esp_err_t app_nvs_set(app_nvs_t *store, const char *namespace_name,
                      const char *key, const app_nvs_value_t *value,
                      app_nvs_condition_t condition,
                      const char *expected_etag, bool *out_created,
                      char out_etag[APP_NVS_ETAG_HEX_SIZE])
{
    if (store != &s_store || !store->mutex || !value || !out_created ||
        !out_etag) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = validate_write(namespace_name, key, value, condition);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    err = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(store->mutex);
        return err;
    }
    bool exists = false;
    nvs_type_t existing_type = NVS_TYPE_ANY;
    err = check_condition_locked(handle, namespace_name, key, condition,
                                 expected_etag, &exists, &existing_type);
    if (err == ESP_OK && exists && existing_type != value->type) {
        err = nvs_erase_key(handle, key);
    }
    if (err == ESP_OK) {
        err = set_value_locked(handle, key, value);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    xSemaphoreGive(store->mutex);
    if (err == ESP_OK) {
        *out_created = !exists;
        err = value_etag(value, out_etag);
    }
    return err;
}

esp_err_t app_nvs_erase(app_nvs_t *store, const char *namespace_name,
                        const char *key, const char *expected_etag)
{
    if (store != &s_store || !store->mutex) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_condition_t condition = expected_etag
                                        ? APP_NVS_CONDITION_MATCH
                                        : APP_NVS_CONDITION_UPSERT;
    esp_err_t err = validate_write(namespace_name, key, NULL, condition);
    if (err != ESP_OK) {
        return err;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    err = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        bool exists = false;
        err = check_condition_locked(handle, namespace_name, key, condition,
                                     expected_etag, &exists, NULL);
        if (err == ESP_OK && !exists) {
            err = ESP_ERR_NOT_FOUND;
        }
        if (err == ESP_OK) {
            err = nvs_erase_key(handle, key);
        }
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    xSemaphoreGive(store->mutex);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t app_nvs_erase_namespace(app_nvs_t *store,
                                  const char *namespace_name)
{
    if (store != &s_store || !store->mutex ||
        !name_is_valid(namespace_name, NVS_NS_NAME_MAX_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    app_nvs_policy_t policy = app_nvs_policy(namespace_name);
    if (policy == APP_NVS_POLICY_HIDDEN) {
        return APP_NVS_ERR_HIDDEN;
    }
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(namespace_name, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        err = nvs_open(namespace_name, NVS_READWRITE, &handle);
    }
    if (err == ESP_OK) {
        err = nvs_erase_all(handle);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    xSemaphoreGive(store->mutex);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}

esp_err_t app_nvs_batch(app_nvs_t *store, const char *namespace_name,
                        const app_nvs_batch_operation_t *operations,
                        size_t operation_count,
                        app_nvs_batch_result_t *results)
{
    if (store != &s_store || !store->mutex || !operations ||
        operation_count == 0 || !results) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < operation_count; ++i) {
        esp_err_t err = validate_write(namespace_name, operations[i].key,
                                       operations[i].erase
                                           ? NULL : operations[i].value,
                                       operations[i].condition);
        if (err != ESP_OK || (!operations[i].erase && !operations[i].value)) {
            return err == ESP_OK ? ESP_ERR_INVALID_ARG : err;
        }
        for (size_t j = 0; j < i; ++j) {
            if (strcmp(operations[i].key, operations[j].key) == 0) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    bool *exists = calloc(operation_count, sizeof(*exists));
    nvs_type_t *types = calloc(operation_count, sizeof(*types));
    if (!exists || !types) {
        free(exists);
        free(types);
        return ESP_ERR_NO_MEM;
    }
    memset(results, 0, operation_count * sizeof(*results));
    xSemaphoreTake(store->mutex, portMAX_DELAY);
    nvs_handle_t handle;
    esp_err_t err = nvs_open(namespace_name, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(store->mutex);
        free(exists);
        free(types);
        return err;
    }

    for (size_t i = 0; i < operation_count && err == ESP_OK; ++i) {
        err = check_condition_locked(handle, namespace_name,
                                     operations[i].key,
                                     operations[i].condition,
                                     operations[i].expected_etag,
                                     &exists[i], &types[i]);
        if (err == ESP_OK && operations[i].erase && !exists[i]) {
            err = ESP_ERR_NOT_FOUND;
        }
    }
    for (size_t i = 0; i < operation_count && err == ESP_OK; ++i) {
        if (operations[i].erase) {
            err = nvs_erase_key(handle, operations[i].key);
        } else {
            if (exists[i] && types[i] != operations[i].value->type) {
                err = nvs_erase_key(handle, operations[i].key);
            }
            if (err == ESP_OK) {
                err = set_value_locked(handle, operations[i].key,
                                       operations[i].value);
            }
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    xSemaphoreGive(store->mutex);

    if (err == ESP_OK) {
        for (size_t i = 0; i < operation_count; ++i) {
            results[i].created = !exists[i] && !operations[i].erase;
            if (!operations[i].erase) {
                err = value_etag(operations[i].value, results[i].etag);
                if (err != ESP_OK) {
                    break;
                }
            }
        }
    }
    free(exists);
    free(types);
    return err == ESP_ERR_NVS_NOT_FOUND ? ESP_ERR_NOT_FOUND : err;
}
