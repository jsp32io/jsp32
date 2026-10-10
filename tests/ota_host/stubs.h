#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_NOT_FOUND 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_OTA_ROLLBACK_FAILED 5
#define ESP_OTA_IMG_UNDEFINED 0
#define ESP_OTA_IMG_PENDING_VERIFY 1
typedef int esp_ota_img_states_t;
typedef struct { char project_name[32], version[32]; uint32_t secure_version; } esp_app_desc_t;
typedef struct { char label[17]; uint32_t address, size; } esp_partition_t;
typedef int esp_ota_handle_t;
typedef int SemaphoreHandle_t;
typedef int psa_hash_operation_t;
#define OTA_WITH_SEQUENTIAL_WRITES ((size_t)-2)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
#define MALLOC_CAP_8BIT 4
#define portMAX_DELAY 0
#define PSA_SUCCESS 0
#define PSA_ALG_SHA_256 1
size_t strlcpy(char *, const char *, size_t);
const char *esp_err_to_name(esp_err_t);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
void vSemaphoreDelete(SemaphoreHandle_t);
void xSemaphoreTake(SemaphoreHandle_t, int);
void xSemaphoreGive(SemaphoreHandle_t);
void *heap_caps_malloc(size_t, int);
const esp_partition_t *esp_ota_get_running_partition(void);
const esp_partition_t *esp_ota_get_boot_partition(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_get_partition_description(const esp_partition_t *, esp_app_desc_t *);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback(void);
bool esp_ota_check_rollback_is_possible(void);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_abort(esp_ota_handle_t);
int psa_crypto_init(void);
psa_hash_operation_t psa_hash_operation_init(void);
int psa_hash_setup(psa_hash_operation_t *, int);
int psa_hash_update(psa_hash_operation_t *, const void *, size_t);
int psa_hash_finish(psa_hash_operation_t *, unsigned char *, size_t, size_t *);
int psa_hash_abort(psa_hash_operation_t *);
