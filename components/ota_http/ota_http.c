#include "ota_http.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define OTA_HTTP_OWNER "ota_http"
#define OTA_ROOT "/api/v1/ota"
#define UPLOAD_BUFFER_SIZE 4096

#if CONFIG_OTA_HTTP_ENABLED

struct ota_http {
    app_http_server_t *http_server;
    app_ota_t *ota;
    portMUX_TYPE lock;
    uint32_t active_uploads;
    bool stopping;
};

static esp_err_t send_json(httpd_req_t *request, const char *status,
                           cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) {
        httpd_resp_set_status(request, HTTPD_500);
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request,
            "{\"ok\":false,\"error\":{\"code\":\"OTA_NO_MEMORY\","
            "\"name\":\"OutOfMemoryError\","
            "\"message\":\"cannot encode response\"}}");
    }
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, strlen(body));
    cJSON_free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, const char *status,
                            const char *code, const char *name,
                            const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = cJSON_CreateObject();
    if (!root || !error) {
        cJSON_Delete(root);
        cJSON_Delete(error);
        httpd_resp_set_status(request, HTTPD_500);
        return httpd_resp_sendstr(request, "Internal Server Error");
    }
    cJSON_AddFalseToObject(root, "ok");
    cJSON_AddItemToObject(root, "error", error);
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "name", name);
    cJSON_AddStringToObject(error, "message", message);
    return send_json(request, status, root);
}

static esp_err_t send_ota_error(httpd_req_t *request, esp_err_t err)
{
    if (err == APP_OTA_ERR_BUSY) {
        return send_error(request, "409 Conflict", "OTA_BUSY", "BusyError",
                          "an OTA operation is already in progress");
    }
    if (err == APP_OTA_ERR_PENDING_CONFIRM) {
        return send_error(request, "409 Conflict", "OTA_CONFIRM_REQUIRED",
                          "StateError",
                          "confirm or roll back the running firmware first");
    }
    if (err == APP_OTA_ERR_UPDATE_READY) {
        return send_error(request, "409 Conflict", "OTA_UPDATE_READY",
                          "StateError", "an update is already ready to boot");
    }
    if (err == APP_OTA_ERR_CHECKSUM) {
        return send_error(request, "422 Unprocessable Content",
                          "OTA_CHECKSUM_MISMATCH", "ChecksumError",
                          "firmware checksum does not match");
    }
    if (err == APP_OTA_ERR_WRONG_PROJECT) {
        return send_error(request, "422 Unprocessable Content",
                          "OTA_WRONG_PROJECT", "FirmwareError",
                          "firmware project name must be jsp32");
    }
    if (err == APP_OTA_ERR_NO_ROLLBACK ||
        err == ESP_ERR_OTA_ROLLBACK_FAILED) {
        return send_error(request, "409 Conflict", "OTA_NO_ROLLBACK",
                          "StateError", "no valid rollback firmware exists");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_error(request, "413 Content Too Large",
                          "OTA_INVALID_SIZE", "RequestTooLargeError",
                          "firmware does not fit the update partition");
    }
    if (err == ESP_ERR_TIMEOUT) {
        return send_error(request, "408 Request Timeout",
                          "OTA_UPLOAD_TIMEOUT", "RequestError",
                          "firmware upload timed out");
    }
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_RESPONSE) {
        return send_error(request, HTTPD_400, "OTA_UPLOAD_FAILED",
                          "RequestError", "cannot read the firmware upload");
    }
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE ||
        err == ESP_ERR_OTA_PARTITION_CONFLICT) {
        return send_error(request, "409 Conflict", "OTA_INVALID_STATE",
                          "StateError", "firmware state prevents this operation");
    }
    if (err == ESP_ERR_OTA_VALIDATE_FAILED || err == ESP_ERR_NOT_FOUND) {
        return send_error(request, "422 Unprocessable Content",
                          "OTA_INVALID_FIRMWARE", "FirmwareError",
                          "firmware image is invalid");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(request, HTTPD_500, "OTA_NO_MEMORY",
                          "OutOfMemoryError", "not enough working memory");
    }
    return send_error(request, HTTPD_500, "OTA_FAILED", "OtaError",
                      esp_err_to_name(err));
}

static const char *image_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

static cJSON *partition_json(const app_ota_partition_info_t *partition)
{
    if (!partition->present) {
        return cJSON_CreateNull();
    }
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }
    cJSON_AddStringToObject(json, "partition", partition->label);
    cJSON_AddNumberToObject(json, "address", partition->address);
    cJSON_AddNumberToObject(json, "size", partition->size);
    if (partition->has_description) {
        cJSON_AddStringToObject(json, "project", partition->project_name);
        cJSON_AddStringToObject(json, "version", partition->version);
        cJSON_AddNumberToObject(json, "secureVersion",
                                partition->secure_version);
    }
    return json;
}

static const char *status_name(const app_ota_status_t *status)
{
    if (status->phase == APP_OTA_PHASE_RECEIVING) return "receiving";
    if (status->phase == APP_OTA_PHASE_VERIFYING) return "verifying";
    if (status->update_ready) return "ready";
    if (status->running_state_known &&
        status->running_state == ESP_OTA_IMG_PENDING_VERIFY) {
        return "pending_confirm";
    }
    return "idle";
}

static esp_err_t status_handler(httpd_req_t *request)
{
    ota_http_t *http = request->user_ctx;
    app_ota_status_t status;
    esp_err_t err = app_ota_get_status(http->ota, &status);
    if (err != ESP_OK) {
        return send_ota_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *running = partition_json(&status.running);
    cJSON *boot = partition_json(&status.boot);
    cJSON *next = partition_json(&status.next);
    cJSON *rollback = cJSON_CreateObject();
    if (!root || !running || !boot || !next || !rollback) {
        cJSON_Delete(root);
        cJSON_Delete(running);
        cJSON_Delete(boot);
        cJSON_Delete(next);
        cJSON_Delete(rollback);
        return send_ota_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "state", status_name(&status));
    cJSON_AddItemToObject(root, "running", running);
    cJSON_AddStringToObject(running, "state",
                            image_state_name(status.running_state));
    cJSON_AddItemToObject(root, "boot", boot);
    cJSON_AddItemToObject(root, "next", next);
    if (status.phase != APP_OTA_PHASE_IDLE) {
        cJSON *upload = cJSON_CreateObject();
        if (!upload) {
            cJSON_Delete(root);
            return send_ota_error(request, ESP_ERR_NO_MEM);
        }
        cJSON_AddStringToObject(upload, "id", status.upload_id);
        cJSON_AddNumberToObject(upload, "received", status.received);
        cJSON_AddNumberToObject(upload, "written", status.written);
        cJSON_AddNumberToObject(upload, "total", status.total);
        double percent = status.total
            ? (double)status.received * 100.0 / (double)status.total : 0;
        cJSON_AddNumberToObject(upload, "percent", percent);
        cJSON_AddNumberToObject(upload, "writtenPercent", status.total
            ? (double)status.written * 100.0 / (double)status.total : 0);
        cJSON_AddItemToObject(root, "upload", upload);
    } else {
        cJSON_AddNullToObject(root, "upload");
    }
    cJSON_AddItemToObject(root, "rollback", rollback);
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    cJSON_AddTrueToObject(rollback, "enabled");
#else
    cJSON_AddFalseToObject(rollback, "enabled");
#endif
    cJSON_AddBoolToObject(rollback, "possible", status.rollback_possible);
    if (status.last_error[0]) {
        cJSON_AddStringToObject(root, "lastError", status.last_error);
    } else {
        cJSON_AddNullToObject(root, "lastError");
    }
    return send_json(request, HTTPD_200, root);
}

static bool sha256_is_valid(const char *value)
{
    if (!value || strlen(value) != 64) {
        return false;
    }
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)value[i])) {
            return false;
        }
    }
    return true;
}

static bool get_header(httpd_req_t *request, const char *name,
                       char *output, size_t output_size)
{
    size_t length = httpd_req_get_hdr_value_len(request, name);
    return length > 0 && length < output_size &&
           httpd_req_get_hdr_value_str(request, name, output,
                                       output_size) == ESP_OK;
}

static bool upload_try_acquire(ota_http_t *http)
{
    if (!app_http_server_worker_try_acquire(http->http_server)) {
        return false;
    }
    bool acquired = false;
    portENTER_CRITICAL(&http->lock);
    if (!http->stopping && http->active_uploads == 0) {
        http->active_uploads = 1;
        acquired = true;
    }
    portEXIT_CRITICAL(&http->lock);
    if (!acquired) {
        app_http_server_worker_release(http->http_server);
    }
    return acquired;
}

static void upload_release(ota_http_t *http)
{
    portENTER_CRITICAL(&http->lock);
    http->active_uploads = 0;
    portEXIT_CRITICAL(&http->lock);
    app_http_server_worker_release(http->http_server);
}

static void upload_task(void *argument)
{
    httpd_req_t *request = argument;
    ota_http_t *http = request->user_ctx;
    char expected_sha256[APP_OTA_SHA256_HEX_SIZE] = {0};
    get_header(request, "X-Content-SHA256", expected_sha256,
               sizeof(expected_sha256));
    char upload_id[APP_OTA_UPLOAD_ID_SIZE] = {0};
    get_header(request, "X-Upload-ID", upload_id, sizeof(upload_id));
    app_ota_upload_t *upload = NULL;
    esp_err_t err = app_ota_upload_begin(http->ota, request->content_len,
                                         expected_sha256, upload_id, &upload);
    unsigned char *buffer = NULL;
    if (err == ESP_OK) {
        buffer = malloc(UPLOAD_BUFFER_SIZE);
        if (!buffer) {
            err = ESP_ERR_NO_MEM;
        }
    }
    size_t received = 0;
    unsigned timeout_count = 0;
    while (err == ESP_OK && received < request->content_len) {
        size_t remaining = request->content_len - received;
        size_t wanted = remaining < UPLOAD_BUFFER_SIZE
                            ? remaining
                            : UPLOAD_BUFFER_SIZE;
        int count = httpd_req_recv(request, (char *)buffer, wanted);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeout_count >= 3) {
                err = ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if (count <= 0) {
            err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        timeout_count = 0;
        err = app_ota_upload_write(upload, buffer, (size_t)count);
        received += (size_t)count;
    }
    free(buffer);
    if (err == ESP_OK) {
        char actual_sha256[APP_OTA_SHA256_HEX_SIZE];
        app_ota_partition_info_t partition;
        err = app_ota_upload_finish(upload, actual_sha256, &partition);
        upload = NULL;
        if (err == ESP_OK) {
            cJSON *root = cJSON_CreateObject();
            cJSON *firmware = partition_json(&partition);
            if (!root || !firmware) {
                cJSON_Delete(root);
                cJSON_Delete(firmware);
                send_ota_error(request, ESP_ERR_NO_MEM);
            } else {
                cJSON_AddTrueToObject(root, "ok");
                cJSON_AddStringToObject(root, "state", "ready");
                cJSON_AddStringToObject(root, "sha256", actual_sha256);
                cJSON_AddItemToObject(root, "firmware", firmware);
                send_json(request, HTTPD_200, root);
            }
        }
    }
    if (err != ESP_OK) {
        if (upload) {
            app_ota_upload_abort(upload, err);
        }
        send_ota_error(request, err);
    }
    httpd_req_async_handler_complete(request);
    upload_release(http);
    vTaskDelete(NULL);
}

static esp_err_t upload_handler(httpd_req_t *request)
{
    if (request->content_len == 0) {
        return send_error(request, HTTPD_400, "OTA_LENGTH_REQUIRED",
                          "ValidationError",
                          "a non-empty Content-Length is required");
    }
    char content_type[48];
    if (!get_header(request, "Content-Type", content_type,
                    sizeof(content_type)) ||
        strcasecmp(content_type, "application/octet-stream") != 0) {
        return send_error(request, "415 Unsupported Media Type",
                          "OTA_CONTENT_TYPE", "ValidationError",
                          "Content-Type must be application/octet-stream");
    }
    char checksum[APP_OTA_SHA256_HEX_SIZE];
    if (!get_header(request, "X-Content-SHA256", checksum,
                    sizeof(checksum)) || !sha256_is_valid(checksum)) {
        return send_error(request, HTTPD_400, "OTA_INVALID_CHECKSUM",
                          "ValidationError",
                          "X-Content-SHA256 must contain 64 hex digits");
    }
    size_t id_length = httpd_req_get_hdr_value_len(request, "X-Upload-ID");
    if (id_length > 0) {
        char upload_id[APP_OTA_UPLOAD_ID_SIZE];
        if (id_length != APP_OTA_UPLOAD_ID_SIZE - 1 ||
            !get_header(request, "X-Upload-ID", upload_id, sizeof(upload_id)) ||
            strspn(upload_id, "0123456789abcdef") != id_length) {
            return send_error(request, HTTPD_400, "OTA_INVALID_UPLOAD_ID",
                              "ValidationError", "X-Upload-ID must contain 32 lowercase hex digits");
        }
    }
    if (!upload_try_acquire(request->user_ctx)) {
        return send_error(request, "429 Too Many Requests", "OTA_BUSY",
                          "BusyError", "an OTA upload is already running");
    }
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        upload_release(request->user_ctx);
        return err;
    }
    if (xTaskCreate(upload_task, "ota_http", CONFIG_OTA_HTTP_TASK_STACK_SIZE,
                    async_request, CONFIG_OTA_HTTP_TASK_PRIORITY,
                    NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_request);
        upload_release(request->user_ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void reboot_task(void *argument)
{
    (void)argument;
    vTaskDelay(pdMS_TO_TICKS(CONFIG_OTA_HTTP_REBOOT_DELAY_MS));
    esp_restart();
}

static esp_err_t schedule_reboot(httpd_req_t *request, const char *state)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_ota_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "state", state);
    if (xTaskCreate(reboot_task, "ota_reboot", 2048, NULL,
                    CONFIG_OTA_HTTP_TASK_PRIORITY, NULL) != pdPASS) {
        cJSON_Delete(root);
        return send_ota_error(request, ESP_ERR_NO_MEM);
    }
    return send_json(request, "202 Accepted", root);
}

static esp_err_t action_handler(httpd_req_t *request)
{
    ota_http_t *http = request->user_ctx;
    if (strcmp(request->uri, OTA_ROOT "/confirm") == 0) {
        bool changed = false;
        esp_err_t err = app_ota_confirm(http->ota, &changed);
        if (err != ESP_OK) {
            return send_ota_error(request, err);
        }
        cJSON *root = cJSON_CreateObject();
        if (!root) {
            return send_ota_error(request, ESP_ERR_NO_MEM);
        }
        cJSON_AddTrueToObject(root, "ok");
        cJSON_AddStringToObject(root, "state", "valid");
        cJSON_AddBoolToObject(root, "changed", changed);
        return send_json(request, HTTPD_200, root);
    }
    if (strcmp(request->uri, OTA_ROOT "/rollback") == 0) {
        esp_err_t err = app_ota_prepare_rollback(http->ota);
        if (err != ESP_OK) {
            return send_ota_error(request, err);
        }
        return schedule_reboot(request, "rolling_back");
    }
    esp_err_t err = app_ota_prepare_reboot(http->ota);
    if (err != ESP_OK) {
        return send_ota_error(request, err);
    }
    return schedule_reboot(request, "rebooting");
}

esp_err_t ota_http_start(const ota_http_config_t *config,
                         ota_http_t **out_http)
{
    if (!config || !config->http_server || !config->ota || !out_http) {
        return ESP_ERR_INVALID_ARG;
    }
    ota_http_t *http = calloc(1, sizeof(*http));
    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    http->http_server = config->http_server;
    http->ota = config->ota;
    http->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    const httpd_uri_t routes[] = {
        {.uri = OTA_ROOT, .method = HTTP_GET,
         .handler = status_handler, .user_ctx = http},
        {.uri = OTA_ROOT "/firmware", .method = HTTP_PUT,
         .handler = upload_handler, .user_ctx = http},
        {.uri = OTA_ROOT "/reboot", .method = HTTP_POST,
         .handler = action_handler, .user_ctx = http},
        {.uri = OTA_ROOT "/confirm", .method = HTTP_POST,
         .handler = action_handler, .user_ctx = http},
        {.uri = OTA_ROOT "/rollback", .method = HTTP_POST,
         .handler = action_handler, .user_ctx = http},
    };
    esp_err_t err = app_http_server_register(
        http->http_server, OTA_HTTP_OWNER, routes,
        sizeof(routes) / sizeof(routes[0]));
    if (err != ESP_OK) {
        free(http);
        return err;
    }
    *out_http = http;
    return ESP_OK;
}

esp_err_t ota_http_stop(ota_http_t *http, TickType_t wait_ticks)
{
    if (!http) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&http->lock);
    http->stopping = true;
    portEXIT_CRITICAL(&http->lock);
    esp_err_t err = app_http_server_unregister_owner(http->http_server,
                                                      OTA_HTTP_OWNER);
    if (err != ESP_OK) {
        return err;
    }
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&http->lock);
        uint32_t active = http->active_uploads;
        portEXIT_CRITICAL(&http->lock);
        if (active == 0) {
            free(http);
            return ESP_OK;
        }
        if (wait_ticks == 0 ||
            xTaskGetTickCount() - started >= wait_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

#else

esp_err_t ota_http_start(const ota_http_config_t *config,
                         ota_http_t **out_http)
{
    (void)config;
    (void)out_http;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ota_http_stop(ota_http_t *http, TickType_t wait_ticks)
{
    (void)http;
    (void)wait_ticks;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
