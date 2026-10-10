#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "nvs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_NVS_ETAG_HEX_SIZE 65

#define APP_NVS_ERR_PRECONDITION 0x10101
#define APP_NVS_ERR_MANAGED      0x10102
#define APP_NVS_ERR_HIDDEN       0x10103

typedef struct app_nvs app_nvs_t;

typedef enum {
    APP_NVS_POLICY_USER,
    APP_NVS_POLICY_MANAGED,
    APP_NVS_POLICY_HIDDEN,
} app_nvs_policy_t;

typedef enum {
    APP_NVS_CONDITION_UPSERT,
    APP_NVS_CONDITION_CREATE_ONLY,
    APP_NVS_CONDITION_MATCH,
} app_nvs_condition_t;

typedef struct {
    nvs_type_t type;
    union {
        int8_t i8;
        uint8_t u8;
        int16_t i16;
        uint16_t u16;
        int32_t i32;
        uint32_t u32;
        int64_t i64;
        uint64_t u64;
        float f32;
        double f64;
        struct {
            void *data;
            size_t size;
        } bytes;
    } data;
} app_nvs_value_t;

typedef struct {
    char name[NVS_NS_NAME_MAX_SIZE];
    app_nvs_policy_t policy;
    size_t used_entries;
} app_nvs_namespace_info_t;

typedef struct {
    char key[NVS_KEY_NAME_MAX_SIZE];
    nvs_type_t type;
    size_t size;
    bool sensitive;
    bool readable;
    bool writable;
} app_nvs_entry_info_t;

typedef struct {
    bool erase;
    const char *key;
    const app_nvs_value_t *value;
    app_nvs_condition_t condition;
    const char *expected_etag;
} app_nvs_batch_operation_t;

typedef struct {
    bool created;
    char etag[APP_NVS_ETAG_HEX_SIZE];
} app_nvs_batch_result_t;

esp_err_t app_nvs_init(app_nvs_t **out_store);
esp_err_t app_nvs_stats(app_nvs_t *store, nvs_stats_t *out_stats);
app_nvs_policy_t app_nvs_policy(const char *namespace_name);

esp_err_t app_nvs_list_namespaces(app_nvs_t *store, const char *prefix,
                                  const char *after,
                                  app_nvs_namespace_info_t *entries,
                                  size_t capacity, size_t *out_count,
                                  bool *out_has_more);
esp_err_t app_nvs_namespace_info(app_nvs_t *store, const char *namespace_name,
                                 app_nvs_namespace_info_t *out_info);
esp_err_t app_nvs_list_entries(app_nvs_t *store, const char *namespace_name,
                               const char *prefix, const char *after,
                               app_nvs_entry_info_t *entries,
                               size_t capacity, size_t *out_count,
                               bool *out_has_more);

esp_err_t app_nvs_get(app_nvs_t *store, const char *namespace_name,
                      const char *key, app_nvs_value_t *out_value,
                      char out_etag[APP_NVS_ETAG_HEX_SIZE]);
void app_nvs_value_free(app_nvs_value_t *value);

esp_err_t app_nvs_set(app_nvs_t *store, const char *namespace_name,
                      const char *key, const app_nvs_value_t *value,
                      app_nvs_condition_t condition,
                      const char *expected_etag, bool *out_created,
                      char out_etag[APP_NVS_ETAG_HEX_SIZE]);
esp_err_t app_nvs_erase(app_nvs_t *store, const char *namespace_name,
                        const char *key, const char *expected_etag);
esp_err_t app_nvs_erase_namespace(app_nvs_t *store,
                                  const char *namespace_name);
esp_err_t app_nvs_batch(app_nvs_t *store, const char *namespace_name,
                        const app_nvs_batch_operation_t *operations,
                        size_t operation_count,
                        app_nvs_batch_result_t *results);

const char *app_nvs_type_name(nvs_type_t type);

#ifdef __cplusplus
}
#endif
