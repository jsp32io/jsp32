#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_flash_partitions.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_OTA_SHA256_HEX_SIZE 65
#define APP_OTA_UPLOAD_ID_SIZE 33

#define APP_OTA_ERR_BASE               0x7000
#define APP_OTA_ERR_BUSY               (APP_OTA_ERR_BASE + 1)
#define APP_OTA_ERR_PENDING_CONFIRM    (APP_OTA_ERR_BASE + 2)
#define APP_OTA_ERR_UPDATE_READY       (APP_OTA_ERR_BASE + 3)
#define APP_OTA_ERR_CHECKSUM           (APP_OTA_ERR_BASE + 4)
#define APP_OTA_ERR_WRONG_PROJECT      (APP_OTA_ERR_BASE + 5)
#define APP_OTA_ERR_NO_ROLLBACK        (APP_OTA_ERR_BASE + 7)

typedef struct app_ota app_ota_t;
typedef struct app_ota_upload app_ota_upload_t;

typedef enum {
    APP_OTA_PHASE_IDLE,
    APP_OTA_PHASE_RECEIVING,
    APP_OTA_PHASE_VERIFYING,
} app_ota_phase_t;

typedef struct {
    bool present;
    char label[17];
    uint32_t address;
    uint32_t size;
    bool has_description;
    char project_name[sizeof(((esp_app_desc_t *)0)->project_name) + 1];
    char version[sizeof(((esp_app_desc_t *)0)->version) + 1];
    uint32_t secure_version;
} app_ota_partition_info_t;

typedef struct {
    app_ota_phase_t phase;
    size_t received;
    size_t written; /* Image bytes whose Flash write has completed; excludes padding. */
    size_t total;
    char upload_id[APP_OTA_UPLOAD_ID_SIZE];
    app_ota_partition_info_t running;
    app_ota_partition_info_t boot;
    app_ota_partition_info_t next;
    bool running_state_known;
    esp_ota_img_states_t running_state;
    bool rollback_possible;
    bool update_ready;
    char last_error[48];
} app_ota_status_t;

esp_err_t app_ota_init(app_ota_t **out_ota);
esp_err_t app_ota_get_status(app_ota_t *ota, app_ota_status_t *out_status);

esp_err_t app_ota_upload_begin(app_ota_t *ota, size_t image_size,
                               const char *expected_sha256, const char *upload_id,
                               app_ota_upload_t **out_upload);
esp_err_t app_ota_upload_write(app_ota_upload_t *upload,
                               const void *data, size_t size);
esp_err_t app_ota_upload_finish(app_ota_upload_t *upload,
                                char actual_sha256[APP_OTA_SHA256_HEX_SIZE],
                                app_ota_partition_info_t *out_partition);
void app_ota_upload_abort(app_ota_upload_t *upload, esp_err_t reason);

esp_err_t app_ota_confirm(app_ota_t *ota, bool *out_changed);
esp_err_t app_ota_prepare_reboot(app_ota_t *ota);
esp_err_t app_ota_prepare_rollback(app_ota_t *ota);

#ifdef __cplusplus
}
#endif
