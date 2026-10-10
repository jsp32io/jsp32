#include "app_ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_ota_ops.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "psa/crypto.h"

#define EXPECTED_PROJECT_NAME "jsp32"
#define OTA_FLASH_BLOCK_SIZE (64U * 1024U)

struct app_ota {
    SemaphoreHandle_t mutex;
    app_ota_phase_t phase;
    size_t received;
    size_t written;
    size_t total;
    char upload_id[APP_OTA_UPLOAD_ID_SIZE];
    char last_error[48];
};

struct app_ota_upload {
    app_ota_t *ota;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    psa_hash_operation_t hash;
    size_t total;
    size_t received;
    unsigned char expected_sha256[32];
    unsigned char *pending;
    size_t pending_size;
    bool handle_open;
    bool hash_open;
};

static app_ota_t s_ota;

static esp_err_t ensure_initial_running_entry(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (!running || !boot || running != boot) {
        return ESP_OK;
    }
    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err == ESP_OK) {
        return state == ESP_OTA_IMG_UNDEFINED
            ? esp_ota_mark_app_valid_cancel_rollback() : ESP_OK;
    }
    if (err != ESP_ERR_NOT_FOUND) {
        return err;
    }

    /* A factory-less first flash boots ota_0 with erased otadata. Record that
     * running image as the initial valid image so the first OTA can roll back
     * to it. Undefined entries from older non-rollback builds are normalized
     * above. This is bootstrap only; pending OTA images already have state. */
    err = esp_ota_set_boot_partition(running);
    if (err != ESP_OK) {
        return err;
    }
    return esp_ota_mark_app_valid_cancel_rollback();
}

static void copy_fixed_string(char *destination, size_t destination_size,
                              const char *source, size_t source_size)
{
    size_t length = strnlen(source, source_size);
    if (length >= destination_size) {
        length = destination_size - 1;
    }
    memcpy(destination, source, length);
    destination[length] = '\0';
}

static void fill_partition_info(const esp_partition_t *partition,
                                app_ota_partition_info_t *info)
{
    memset(info, 0, sizeof(*info));
    if (!partition) {
        return;
    }
    info->present = true;
    copy_fixed_string(info->label, sizeof(info->label), partition->label,
                      sizeof(partition->label));
    info->address = partition->address;
    info->size = partition->size;
    esp_app_desc_t description;
    if (esp_ota_get_partition_description(partition, &description) == ESP_OK) {
        info->has_description = true;
        copy_fixed_string(info->project_name, sizeof(info->project_name),
                          description.project_name,
                          sizeof(description.project_name));
        copy_fixed_string(info->version, sizeof(info->version),
                          description.version, sizeof(description.version));
        info->secure_version = description.secure_version;
    }
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool decode_sha256(const char *text, unsigned char output[32])
{
    if (!text || strlen(text) != 64) {
        return false;
    }
    for (size_t i = 0; i < 32; ++i) {
        int high = hex_value(text[i * 2]);
        int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        output[i] = (unsigned char)((high << 4) | low);
    }
    return true;
}

static void encode_sha256(const unsigned char digest[32],
                          char output[APP_OTA_SHA256_HEX_SIZE])
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        output[i * 2] = digits[digest[i] >> 4];
        output[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    output[64] = '\0';
}

static void set_idle(app_ota_t *ota, esp_err_t error)
{
    xSemaphoreTake(ota->mutex, portMAX_DELAY);
    ota->phase = APP_OTA_PHASE_IDLE;
    ota->received = 0;
    ota->written = 0;
    ota->total = 0;
    ota->upload_id[0] = '\0';
    if (error == ESP_OK) {
        ota->last_error[0] = '\0';
    } else {
        const char *name = esp_err_to_name(error);
        switch (error) {
        case APP_OTA_ERR_CHECKSUM: name = "OTA_CHECKSUM_MISMATCH"; break;
        case APP_OTA_ERR_WRONG_PROJECT: name = "OTA_WRONG_PROJECT"; break;
        default: break;
        }
        strlcpy(ota->last_error, name, sizeof(ota->last_error));
    }
    xSemaphoreGive(ota->mutex);
}

static void release_upload(app_ota_upload_t *upload, esp_err_t error)
{
    if (!upload) {
        return;
    }
    if (upload->hash_open) {
        psa_hash_abort(&upload->hash);
    }
    if (upload->handle_open) {
        esp_ota_abort(upload->handle);
    }
    set_idle(upload->ota, error);
    free(upload->pending);
    free(upload);
}

esp_err_t app_ota_init(app_ota_t **out_ota)
{
    if (!out_ota) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ota.mutex) {
        s_ota.mutex = xSemaphoreCreateMutex();
        if (!s_ota.mutex) {
            return ESP_ERR_NO_MEM;
        }
        if (psa_crypto_init() != PSA_SUCCESS) {
            vSemaphoreDelete(s_ota.mutex);
            memset(&s_ota, 0, sizeof(s_ota));
            return ESP_FAIL;
        }
        esp_err_t err = ensure_initial_running_entry();
        if (err != ESP_OK) {
            vSemaphoreDelete(s_ota.mutex);
            memset(&s_ota, 0, sizeof(s_ota));
            return err;
        }
    }
    *out_ota = &s_ota;
    return ESP_OK;
}

esp_err_t app_ota_get_status(app_ota_t *ota, app_ota_status_t *out_status)
{
    if (!ota || !out_status) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out_status, 0, sizeof(*out_status));
    xSemaphoreTake(ota->mutex, portMAX_DELAY);
    out_status->phase = ota->phase;
    out_status->received = ota->received;
    out_status->written = ota->written;
    strlcpy(out_status->upload_id, ota->upload_id, sizeof(out_status->upload_id));
    out_status->total = ota->total;
    strlcpy(out_status->last_error, ota->last_error,
            sizeof(out_status->last_error));
    xSemaphoreGive(ota->mutex);

    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    fill_partition_info(running, &out_status->running);
    fill_partition_info(boot, &out_status->boot);
    fill_partition_info(next, &out_status->next);
    if (running &&
        esp_ota_get_state_partition(running, &out_status->running_state) ==
            ESP_OK) {
        out_status->running_state_known = true;
    } else {
        out_status->running_state = ESP_OTA_IMG_UNDEFINED;
    }
    out_status->rollback_possible = esp_ota_check_rollback_is_possible();
    out_status->update_ready = running && boot && running != boot;
    return ESP_OK;
}

esp_err_t app_ota_upload_begin(app_ota_t *ota, size_t image_size,
                               const char *expected_sha256, const char *upload_id,
                               app_ota_upload_t **out_upload)
{
    if (!ota || !out_upload || image_size == 0 || !expected_sha256 ||
        (upload_id && strlen(upload_id) >= APP_OTA_UPLOAD_ID_SIZE)) {
        return ESP_ERR_INVALID_ARG;
    }
    app_ota_upload_t *upload = calloc(1, sizeof(*upload));
    if (!upload) {
        return ESP_ERR_NO_MEM;
    }
    upload->hash = psa_hash_operation_init();
    if (!decode_sha256(expected_sha256, upload->expected_sha256)) {
        free(upload);
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(ota->mutex, portMAX_DELAY);
    if (ota->phase != APP_OTA_PHASE_IDLE) {
        xSemaphoreGive(ota->mutex);
        free(upload);
        return APP_OTA_ERR_BUSY;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (running && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        xSemaphoreGive(ota->mutex);
        free(upload);
        return APP_OTA_ERR_PENDING_CONFIRM;
    }
    if (running && boot && running != boot) {
        xSemaphoreGive(ota->mutex);
        free(upload);
        return APP_OTA_ERR_UPDATE_READY;
    }
    upload->partition = esp_ota_get_next_update_partition(NULL);
    if (!upload->partition || image_size > upload->partition->size) {
        xSemaphoreGive(ota->mutex);
        free(upload);
        return ESP_ERR_INVALID_SIZE;
    }
    ota->phase = APP_OTA_PHASE_RECEIVING;
    ota->received = 0;
    ota->written = 0;
    ota->total = image_size;
    strlcpy(ota->upload_id, upload_id ? upload_id : "", sizeof(ota->upload_id));
    ota->last_error[0] = '\0';
    xSemaphoreGive(ota->mutex);

    upload->ota = ota;
    upload->total = image_size;
    /* Sequential OTA erases the range covered by each esp_ota_write() call.
     * Buffer one physical 64 KiB block so it never asks this chip for a
     * main-array 4 KiB sector erase. */
    upload->pending = heap_caps_malloc(OTA_FLASH_BLOCK_SIZE,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!upload->pending) {
        upload->pending = heap_caps_malloc(OTA_FLASH_BLOCK_SIZE,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!upload->pending) {
        release_upload(upload, ESP_ERR_NO_MEM);
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_ota_begin(upload->partition, OTA_WITH_SEQUENTIAL_WRITES,
                                  &upload->handle);
    if (err != ESP_OK) {
        release_upload(upload, err);
        return err;
    }
    upload->handle_open = true;
    if (psa_hash_setup(&upload->hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        release_upload(upload, ESP_FAIL);
        return ESP_FAIL;
    }
    upload->hash_open = true;
    *out_upload = upload;
    return ESP_OK;
}

esp_err_t app_ota_upload_write(app_ota_upload_t *upload,
                               const void *data, size_t size)
{
    if (!upload || (!data && size > 0) ||
        size > upload->total - upload->received) {
        return ESP_ERR_INVALID_ARG;
    }
    if (size == 0) {
        return ESP_OK;
    }
    if (psa_hash_update(&upload->hash, data, size) != PSA_SUCCESS) {
        return ESP_FAIL;
    }

    const unsigned char *source = data;
    size_t remaining = size;
    while (remaining > 0) {
        size_t copy = OTA_FLASH_BLOCK_SIZE - upload->pending_size;
        if (copy > remaining) {
            copy = remaining;
        }
        memcpy(upload->pending + upload->pending_size, source, copy);
        upload->pending_size += copy;
        source += copy;
        remaining -= copy;
        if (upload->pending_size != OTA_FLASH_BLOCK_SIZE) {
            continue;
        }
        esp_err_t err = esp_ota_write(upload->handle, upload->pending,
                                      OTA_FLASH_BLOCK_SIZE);
        if (err != ESP_OK) {
            return err;
        }
        upload->pending_size = 0;
        xSemaphoreTake(upload->ota->mutex, portMAX_DELAY);
        upload->ota->written += OTA_FLASH_BLOCK_SIZE;
        upload->ota->received = upload->received + size - remaining;
        xSemaphoreGive(upload->ota->mutex);
    }
    upload->received += size;
    xSemaphoreTake(upload->ota->mutex, portMAX_DELAY);
    upload->ota->received = upload->received;
    xSemaphoreGive(upload->ota->mutex);
    return ESP_OK;
}

esp_err_t app_ota_upload_finish(app_ota_upload_t *upload,
                                char actual_sha256[APP_OTA_SHA256_HEX_SIZE],
                                app_ota_partition_info_t *out_partition)
{
    if (!upload || !actual_sha256 || !out_partition) {
        return ESP_ERR_INVALID_ARG;
    }
    if (upload->received != upload->total) {
        release_upload(upload, ESP_ERR_INVALID_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    if (upload->pending_size > 0) {
        memset(upload->pending + upload->pending_size, 0xff,
               OTA_FLASH_BLOCK_SIZE - upload->pending_size);
        esp_err_t err = esp_ota_write(upload->handle, upload->pending,
                                      OTA_FLASH_BLOCK_SIZE);
        if (err != ESP_OK) {
            release_upload(upload, err);
            return err;
        }
        upload->pending_size = 0;
    }
    xSemaphoreTake(upload->ota->mutex, portMAX_DELAY);
    upload->ota->written = upload->total;
    upload->ota->phase = APP_OTA_PHASE_VERIFYING;
    xSemaphoreGive(upload->ota->mutex);

    unsigned char digest[32];
    size_t digest_size = 0;
    if (psa_hash_finish(&upload->hash, digest, sizeof(digest),
                        &digest_size) != PSA_SUCCESS || digest_size != 32) {
        upload->hash_open = false;
        release_upload(upload, ESP_FAIL);
        return ESP_FAIL;
    }
    upload->hash_open = false;
    encode_sha256(digest, actual_sha256);
    if (memcmp(digest, upload->expected_sha256, sizeof(digest)) != 0) {
        release_upload(upload, APP_OTA_ERR_CHECKSUM);
        return APP_OTA_ERR_CHECKSUM;
    }

    esp_err_t err = esp_ota_end(upload->handle);
    upload->handle_open = false;
    if (err != ESP_OK) {
        release_upload(upload, err);
        return err;
    }
    esp_app_desc_t description;
    err = esp_ota_get_partition_description(upload->partition, &description);
    if (err != ESP_OK) {
        release_upload(upload, err);
        return err;
    }
    char project_name[sizeof(description.project_name) + 1];
    copy_fixed_string(project_name, sizeof(project_name),
                      description.project_name,
                      sizeof(description.project_name));
    if (strcmp(project_name, EXPECTED_PROJECT_NAME) != 0) {
        release_upload(upload, APP_OTA_ERR_WRONG_PROJECT);
        return APP_OTA_ERR_WRONG_PROJECT;
    }
    err = esp_ota_set_boot_partition(upload->partition);
    if (err != ESP_OK) {
        release_upload(upload, err);
        return err;
    }
    fill_partition_info(upload->partition, out_partition);
    set_idle(upload->ota, ESP_OK);
    free(upload->pending);
    free(upload);
    return ESP_OK;
}

void app_ota_upload_abort(app_ota_upload_t *upload, esp_err_t reason)
{
    release_upload(upload, reason);
}

esp_err_t app_ota_confirm(app_ota_t *ota, bool *out_changed)
{
    if (!ota || !out_changed) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ota->mutex, portMAX_DELAY);
    if (ota->phase != APP_OTA_PHASE_IDLE) {
        xSemaphoreGive(ota->mutex);
        return APP_OTA_ERR_BUSY;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t state_err = running
        ? esp_ota_get_state_partition(running, &state) : ESP_ERR_NOT_FOUND;
    if (state_err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        xSemaphoreGive(ota->mutex);
        if (err == ESP_OK) {
            *out_changed = true;
        }
        return err;
    }
    *out_changed = false;
    xSemaphoreGive(ota->mutex);
    return ESP_OK;
}

esp_err_t app_ota_prepare_reboot(app_ota_t *ota)
{
    if (!ota) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ota->mutex, portMAX_DELAY);
    bool busy = ota->phase != APP_OTA_PHASE_IDLE;
    xSemaphoreGive(ota->mutex);
    return busy ? APP_OTA_ERR_BUSY : ESP_OK;
}

esp_err_t app_ota_prepare_rollback(app_ota_t *ota)
{
    if (!ota) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(ota->mutex, portMAX_DELAY);
    if (ota->phase != APP_OTA_PHASE_IDLE) {
        xSemaphoreGive(ota->mutex);
        return APP_OTA_ERR_BUSY;
    }
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (running && boot && running != boot) {
        xSemaphoreGive(ota->mutex);
        return APP_OTA_ERR_UPDATE_READY;
    }
    if (!esp_ota_check_rollback_is_possible()) {
        xSemaphoreGive(ota->mutex);
        return APP_OTA_ERR_NO_ROLLBACK;
    }
    esp_err_t err = esp_ota_mark_app_invalid_rollback();
    xSemaphoreGive(ota->mutex);
    return err == ESP_ERR_OTA_ROLLBACK_FAILED ? APP_OTA_ERR_NO_ROLLBACK : err;
}
