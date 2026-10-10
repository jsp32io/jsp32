#include "fs_http.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define FS_HTTP_OWNER "fs_http"
#define FILE_ROUTE "/api/v1/fs/file"
#define DIRECTORY_ROUTE "/api/v1/fs/directory"
#define MAX_LIST_LIMIT 100
#define TRANSFER_BUFFER_SIZE 2048

#if CONFIG_FS_HTTP_ENABLED

struct fs_http {
    app_http_server_t *http_server;
    app_filesystem_t *filesystem;
    portMUX_TYPE lock;
    uint32_t active_requests;
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
            "{\"ok\":false,\"error\":{\"code\":\"FS_NO_MEMORY\","
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

static esp_err_t send_filesystem_error(httpd_req_t *request, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return send_error(request, HTTPD_400, "FS_INVALID_PATH",
                          "ValidationError", "invalid filesystem path");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_error(request, HTTPD_404, "FS_NOT_FOUND",
                          "NotFoundError", "file not found");
    }
    if (err == APP_FILESYSTEM_ERR_EXISTS) {
        return send_error(request, "409 Conflict", "FS_ALREADY_EXISTS",
                          "StateError", "file or directory already exists");
    }
    if (err == APP_FILESYSTEM_ERR_NOT_EMPTY) {
        return send_error(request, "409 Conflict", "FS_DIRECTORY_NOT_EMPTY",
                          "StateError", "directory is not empty");
    }
    if (err == APP_FILESYSTEM_ERR_IS_DIRECTORY ||
        err == APP_FILESYSTEM_ERR_NOT_DIRECTORY) {
        return send_error(request, "409 Conflict", "FS_WRONG_TYPE",
                          "StateError", "filesystem entry has the wrong type");
    }
    if (err == APP_FILESYSTEM_ERR_PRECONDITION) {
        return send_error(request, "412 Precondition Failed",
                          "FS_PRECONDITION_FAILED", "PreconditionError",
                          "file precondition failed");
    }
    if (err == APP_FILESYSTEM_ERR_CHECKSUM) {
        return send_error(request, "422 Unprocessable Content",
                          "FS_CHECKSUM_MISMATCH", "ChecksumError",
                          "uploaded content checksum does not match");
    }
    if (err == APP_FILESYSTEM_ERR_FULL) {
        return send_error(request, "507 Insufficient Storage", "FS_FULL",
                          "StorageFullError", "insufficient storage");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(request, HTTPD_500, "FS_NO_MEMORY",
                          "OutOfMemoryError", "not enough working memory");
    }
    return send_error(request, HTTPD_500, "FS_IO_ERROR", "FilesystemError",
                      "filesystem operation failed");
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool url_decode(const char *input, char *output, size_t output_size)
{
    size_t written = 0;
    for (size_t i = 0; input[i]; ++i) {
        unsigned char value = (unsigned char)input[i];
        if (value == '%') {
            if (input[i + 1] == '\0' || input[i + 2] == '\0') {
                return false;
            }
            int high = hex_value(input[i + 1]);
            int low = high >= 0 ? hex_value(input[i + 2]) : -1;
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)((high << 4) | low);
            i += 2;
        } else if (value == '+') {
            value = ' ';
        }
        if (value == 0 || written + 1 >= output_size) {
            return false;
        }
        output[written++] = (char)value;
    }
    output[written] = '\0';
    return true;
}

static bool query_value(httpd_req_t *request, const char *key,
                        char *output, size_t output_size, bool required)
{
    size_t query_size = httpd_req_get_url_query_len(request);
    if (query_size == 0 || query_size > 256) {
        return !required;
    }
    char query[257];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char encoded[CONFIG_HTTPD_MAX_URI_LEN + 1];
    esp_err_t err = httpd_query_key_value(query, key, encoded,
                                          sizeof(encoded));
    if (err == ESP_ERR_NOT_FOUND) {
        return !required;
    }
    return err == ESP_OK && url_decode(encoded, output, output_size);
}

static bool query_parameter_present(httpd_req_t *request, const char *key)
{
    size_t query_size = httpd_req_get_url_query_len(request);
    if (query_size == 0 || query_size > 256) {
        return false;
    }
    char query[257];
    char value[CONFIG_HTTPD_MAX_URI_LEN + 1];
    return httpd_req_get_url_query_str(request, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, value, sizeof(value)) !=
               ESP_ERR_NOT_FOUND;
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

static bool header_is_present(httpd_req_t *request, const char *name)
{
    return httpd_req_get_hdr_value_len(request, name) > 0;
}

static bool parse_etag(const char *header,
                       char output[APP_FILESYSTEM_SHA256_HEX_SIZE])
{
    static const char prefix[] = "\"sha256-";
    size_t length = strlen(header);
    if (length != sizeof(prefix) - 1 + 64 + 1 ||
        strncmp(header, prefix, sizeof(prefix) - 1) != 0 ||
        header[length - 1] != '\"') {
        return false;
    }
    memcpy(output, header + sizeof(prefix) - 1, 64);
    output[64] = '\0';
    return sha256_is_valid(output);
}

static void make_etag(const char *sha256, char *output, size_t size)
{
    snprintf(output, size, "\"sha256-%s\"", sha256);
}

static const char *content_type_for_path(const char *path)
{
    const char *extension = strrchr(path, '.');
    if (extension && strcmp(extension, ".js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    if (extension && (strcmp(extension, ".json") == 0)) {
        return "application/json";
    }
    if (extension && (strcmp(extension, ".txt") == 0 ||
                      strcmp(extension, ".log") == 0)) {
        return "text/plain; charset=utf-8";
    }
    return "application/octet-stream";
}

static esp_err_t info_handler(httpd_req_t *request)
{
    fs_http_t *http = request->user_ctx;
    app_filesystem_info_t info;
    esp_err_t err = app_filesystem_info(http->filesystem, &info);
    if (err != ESP_OK) {
        return send_filesystem_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *filesystem = cJSON_CreateObject();
    if (!root || !filesystem) {
        cJSON_Delete(root);
        cJSON_Delete(filesystem);
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "filesystem", filesystem);
    cJSON_AddStringToObject(filesystem, "type", "littlefs");
    cJSON_AddNumberToObject(filesystem, "totalBytes", info.total_bytes);
    cJSON_AddNumberToObject(filesystem, "usedBytes", info.used_bytes);
    cJSON_AddNumberToObject(filesystem, "freeBytes",
                            info.total_bytes - info.used_bytes);
    cJSON_AddNumberToObject(filesystem, "maxNameBytes", info.max_name_bytes);
    cJSON_AddBoolToObject(filesystem, "directories", info.directories);
    cJSON_AddTrueToObject(filesystem, "atomicReplace");
    return send_json(request, HTTPD_200, root);
}

static esp_err_t files_handler(httpd_req_t *request)
{
    fs_http_t *http = request->user_ctx;
    if (httpd_req_get_url_query_len(request) > 256) {
        return send_error(request, HTTPD_400, "FS_INVALID_QUERY",
                          "ValidationError", "query string is too long");
    }
    char limit_text[8] = {0};
    size_t limit = 50;
    if (query_parameter_present(request, "limit")) {
        if (!query_value(request, "limit", limit_text, sizeof(limit_text),
                         true)) {
            return send_error(request, HTTPD_400, "FS_INVALID_LIMIT",
                              "ValidationError", "invalid limit parameter");
        }
        char *end = NULL;
        unsigned long parsed = strtoul(limit_text, &end, 10);
        if (!end || *end || parsed == 0 || parsed > MAX_LIST_LIMIT) {
            return send_error(request, HTTPD_400, "FS_INVALID_LIMIT",
                              "ValidationError", "limit must be from 1 to 100");
        }
        limit = parsed;
    }
    char after[APP_FILESYSTEM_PATH_SIZE] = {0};
    if (query_parameter_present(request, "after") &&
        !query_value(request, "after", after, sizeof(after), true)) {
        return send_error(request, HTTPD_400, "FS_INVALID_CURSOR",
                          "ValidationError", "invalid after cursor");
    }
    char directory[APP_FILESYSTEM_PATH_SIZE] = "/";
    if (query_parameter_present(request, "path") &&
        !query_value(request, "path", directory, sizeof(directory), true)) {
        return send_error(request, HTTPD_400, "FS_INVALID_PATH",
                          "ValidationError", "invalid directory path");
    }
    app_filesystem_file_info_t *entries = calloc(limit, sizeof(*entries));
    if (!entries) {
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    size_t count = 0;
    bool has_more = false;
    esp_err_t err = app_filesystem_list(http->filesystem, directory,
                                       after[0] ? after : NULL,
                                       entries, limit, &count, &has_more);
    if (err != ESP_OK) {
        free(entries);
        return send_filesystem_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *files = cJSON_CreateArray();
    if (!root || !files) {
        cJSON_Delete(root);
        cJSON_Delete(files);
        free(entries);
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "files", files);
    for (size_t i = 0; i < count; ++i) {
        cJSON *file = cJSON_CreateObject();
        if (!file) {
            cJSON_Delete(root);
            free(entries);
            return send_filesystem_error(request, ESP_ERR_NO_MEM);
        }
        cJSON_AddStringToObject(file, "path", entries[i].path);
        cJSON_AddNumberToObject(file, "size", entries[i].size);
        cJSON_AddBoolToObject(file, "directory", entries[i].directory);
        cJSON_AddItemToArray(files, file);
    }
    if (has_more && count > 0) {
        cJSON_AddStringToObject(root, "nextAfter", entries[count - 1].path);
    } else {
        cJSON_AddNullToObject(root, "nextAfter");
    }
    free(entries);
    return send_json(request, HTTPD_200, root);
}

static esp_err_t send_file(httpd_req_t *request, fs_http_t *http,
                           const char *path, bool headers_only)
{
    app_filesystem_reader_t *reader = NULL;
    app_filesystem_file_info_t info;
    char sha256[APP_FILESYSTEM_SHA256_HEX_SIZE];
    esp_err_t err = app_filesystem_read_open(http->filesystem, path, &reader,
                                             &info, sha256);
    if (err != ESP_OK) {
        return send_filesystem_error(request, err);
    }
    char etag[APP_FILESYSTEM_SHA256_HEX_SIZE + 10];
    char size[24];
    make_etag(sha256, etag, sizeof(etag));
    snprintf(size, sizeof(size), "%u", (unsigned)info.size);
    httpd_resp_set_type(request, content_type_for_path(path));
    httpd_resp_set_hdr(request, "ETag", etag);
    httpd_resp_set_hdr(request, "X-File-Size", size);
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    if (headers_only) {
        app_filesystem_read_close(reader);
        return httpd_resp_send(request, NULL, 0);
    }

    unsigned char *buffer = malloc(TRANSFER_BUFFER_SIZE);
    if (!buffer) {
        app_filesystem_read_close(reader);
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    for (;;) {
        size_t count = 0;
        err = app_filesystem_read(reader, buffer, TRANSFER_BUFFER_SIZE, &count);
        if (err != ESP_OK || count == 0) {
            break;
        }
        err = httpd_resp_send_chunk(request, (const char *)buffer, count);
        if (err != ESP_OK) {
            break;
        }
    }
    free(buffer);
    app_filesystem_read_close(reader);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(request, NULL, 0);
    }
    return err;
}

static esp_err_t receive_file(httpd_req_t *request, fs_http_t *http,
                              const char *path)
{
    char if_none_match[8] = {0};
    char if_match_header[96] = {0};
    char expected_existing[APP_FILESYSTEM_SHA256_HEX_SIZE] = {0};
    char expected_content[APP_FILESYSTEM_SHA256_HEX_SIZE] = {0};
    app_filesystem_write_options_t options = {
        .condition = APP_FILESYSTEM_WRITE_UPSERT,
    };
    if (header_is_present(request, "If-None-Match")) {
        if (!get_header(request, "If-None-Match", if_none_match,
                        sizeof(if_none_match)) ||
            strcmp(if_none_match, "*") != 0) {
            return send_error(request, HTTPD_400, "FS_INVALID_ETAG",
                              "ValidationError", "If-None-Match must be *");
        }
        options.condition = APP_FILESYSTEM_WRITE_CREATE_ONLY;
    }
    if (header_is_present(request, "If-Match")) {
        if (!get_header(request, "If-Match", if_match_header,
                        sizeof(if_match_header)) ||
            options.condition != APP_FILESYSTEM_WRITE_UPSERT ||
            !parse_etag(if_match_header, expected_existing)) {
            return send_error(request, HTTPD_400, "FS_INVALID_ETAG",
                              "ValidationError", "invalid conditional ETag");
        }
        options.condition = APP_FILESYSTEM_WRITE_MATCH;
        options.expected_sha256 = expected_existing;
    }
    char checksum_header[APP_FILESYSTEM_SHA256_HEX_SIZE + 1] = {0};
    const char *content_checksum = NULL;
    if (header_is_present(request, "X-Content-SHA256")) {
        if (!get_header(request, "X-Content-SHA256", checksum_header,
                        sizeof(checksum_header)) ||
            !sha256_is_valid(checksum_header)) {
            return send_error(request, HTTPD_400, "FS_INVALID_CHECKSUM",
                              "ValidationError",
                              "X-Content-SHA256 must contain 64 hex digits");
        }
        strlcpy(expected_content, checksum_header, sizeof(expected_content));
        content_checksum = expected_content;
    }

    unsigned char *buffer = malloc(TRANSFER_BUFFER_SIZE);
    if (!buffer) {
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    app_filesystem_writer_t *writer = NULL;
    bool created = false;
    esp_err_t err = app_filesystem_write_begin(http->filesystem, path,
                                               &options, &writer, &created);
    if (err != ESP_OK) {
        free(buffer);
        return send_filesystem_error(request, err);
    }
    size_t received = 0;
    while (received < request->content_len) {
        size_t remaining = request->content_len - received;
        size_t wanted = remaining < TRANSFER_BUFFER_SIZE
                            ? remaining
                            : TRANSFER_BUFFER_SIZE;
        int count = httpd_req_recv(request, (char *)buffer, wanted);
        if (count <= 0) {
            free(buffer);
            app_filesystem_write_abort(writer);
            return send_error(request, HTTPD_400, "FS_UPLOAD_FAILED",
                              "RequestError", "cannot read upload body");
        }
        err = app_filesystem_write(writer, buffer, (size_t)count);
        if (err != ESP_OK) {
            free(buffer);
            app_filesystem_write_abort(writer);
            return send_filesystem_error(request, err);
        }
        received += (size_t)count;
    }
    free(buffer);
    char sha256[APP_FILESYSTEM_SHA256_HEX_SIZE];
    err = app_filesystem_write_commit(writer, content_checksum, sha256);
    if (err != ESP_OK) {
        return send_filesystem_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *file = cJSON_CreateObject();
    if (!root || !file) {
        cJSON_Delete(root);
        cJSON_Delete(file);
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "file", file);
    cJSON_AddStringToObject(file, "path", path);
    cJSON_AddNumberToObject(file, "size", request->content_len);
    cJSON_AddStringToObject(file, "sha256", sha256);
    cJSON_AddBoolToObject(file, "created", created);
    cJSON_AddBoolToObject(file, "restartRequired",
                          strcmp(path, "/main.js") == 0);
    return send_json(request, created ? "201 Created" : HTTPD_200, root);
}

static esp_err_t delete_file(httpd_req_t *request, fs_http_t *http,
                             const char *path)
{
#if CONFIG_FS_HTTP_PROTECT_MAIN_JS
    if (strcmp(path, "/main.js") == 0) {
        return send_error(request, "409 Conflict", "FS_PROTECTED_FILE",
                          "ProtectedFileError", "/main.js cannot be deleted");
    }
#endif
    char if_match_header[96] = {0};
    char expected[APP_FILESYSTEM_SHA256_HEX_SIZE] = {0};
    const char *expected_pointer = NULL;
    if (header_is_present(request, "If-Match")) {
        if (!get_header(request, "If-Match", if_match_header,
                        sizeof(if_match_header)) ||
            !parse_etag(if_match_header, expected)) {
            return send_error(request, HTTPD_400, "FS_INVALID_ETAG",
                              "ValidationError", "invalid conditional ETag");
        }
        expected_pointer = expected;
    }
    esp_err_t err = app_filesystem_delete(http->filesystem, path,
                                          expected_pointer);
    if (err != ESP_OK) {
        return send_filesystem_error(request, err);
    }
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t create_directory(httpd_req_t *request, fs_http_t *http,
                                   const char *path)
{
    esp_err_t err = app_filesystem_mkdir(http->filesystem, path);
    if (err != ESP_OK) {
        return send_filesystem_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return send_filesystem_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddStringToObject(root, "path", path);
    return send_json(request, "201 Created", root);
}

static bool transfer_try_acquire(fs_http_t *http)
{
    if (!app_http_server_worker_try_acquire(http->http_server)) {
        return false;
    }
    bool acquired = false;
    portENTER_CRITICAL(&http->lock);
    if (!http->stopping &&
        http->active_requests < CONFIG_FS_HTTP_MAX_CONCURRENT_REQUESTS) {
        ++http->active_requests;
        acquired = true;
    }
    portEXIT_CRITICAL(&http->lock);
    if (!acquired) {
        app_http_server_worker_release(http->http_server);
    }
    return acquired;
}

static void transfer_release(fs_http_t *http)
{
    portENTER_CRITICAL(&http->lock);
    if (http->active_requests > 0) {
        --http->active_requests;
    }
    portEXIT_CRITICAL(&http->lock);
    app_http_server_worker_release(http->http_server);
}

static void metadata_task(void *argument)
{
    httpd_req_t *request = argument;
    fs_http_t *http = request->user_ctx;
    if (strncmp(request->uri, "/api/v1/fs/info", 15) == 0) {
        info_handler(request);
    } else {
        files_handler(request);
    }
    httpd_req_async_handler_complete(request);
    transfer_release(http);
    vTaskDelete(NULL);
}

static esp_err_t metadata_handler(httpd_req_t *request)
{
    if (!transfer_try_acquire(request->user_ctx)) {
        return send_error(request, "429 Too Many Requests", "FS_BUSY",
                          "BusyError", "too many filesystem requests");
    }
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        transfer_release(request->user_ctx);
        return err;
    }
    if (xTaskCreate(metadata_task, "fs_http_meta",
                    CONFIG_FS_HTTP_TASK_STACK_SIZE, async_request,
                    CONFIG_FS_HTTP_TASK_PRIORITY, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_request);
        transfer_release(request->user_ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void file_task(void *argument)
{
    httpd_req_t *request = argument;
    fs_http_t *http = request->user_ctx;
    char path[APP_FILESYSTEM_PATH_SIZE] = {0};
    if (!query_value(request, "path", path, sizeof(path), true)) {
        send_error(request, HTTPD_400, "FS_INVALID_PATH", "ValidationError",
                   "path query parameter is required and must be valid");
    } else if (strncmp(request->uri, DIRECTORY_ROUTE,
                       strlen(DIRECTORY_ROUTE)) == 0) {
        create_directory(request, http, path);
    } else if (request->method == HTTP_GET) {
        send_file(request, http, path, false);
    } else if (request->method == HTTP_HEAD) {
        send_file(request, http, path, true);
    } else if (request->method == HTTP_PUT) {
        receive_file(request, http, path);
    } else if (request->method == HTTP_DELETE) {
        delete_file(request, http, path);
    } else {
        send_error(request, "405 Method Not Allowed", "FS_METHOD_NOT_ALLOWED",
                   "MethodNotAllowedError", "unsupported file operation");
    }
    httpd_req_async_handler_complete(request);
    transfer_release(http);
    vTaskDelete(NULL);
}

static esp_err_t file_handler(httpd_req_t *request)
{
    if (request->method == HTTP_PUT &&
        request->content_len > CONFIG_FS_HTTP_MAX_FILE_SIZE) {
        return send_error(request, "413 Content Too Large", "FS_FILE_TOO_LARGE",
                          "RequestTooLargeError",
                          "uploaded file exceeds the configured limit");
    }
    if (!transfer_try_acquire(request->user_ctx)) {
        return send_error(request, "429 Too Many Requests", "FS_BUSY",
                          "BusyError", "too many filesystem requests");
    }
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        transfer_release(request->user_ctx);
        return err;
    }
    if (xTaskCreate(file_task, "fs_http", CONFIG_FS_HTTP_TASK_STACK_SIZE,
                    async_request, CONFIG_FS_HTTP_TASK_PRIORITY,
                    NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_request);
        transfer_release(request->user_ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t fs_http_start(const fs_http_config_t *config, fs_http_t **out_http)
{
    if (!config || !config->http_server || !config->filesystem || !out_http) {
        return ESP_ERR_INVALID_ARG;
    }
    fs_http_t *http = calloc(1, sizeof(*http));
    if (!http) {
        return ESP_ERR_NO_MEM;
    }
    http->http_server = config->http_server;
    http->filesystem = config->filesystem;
    http->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    const httpd_uri_t routes[] = {
        {
            .uri = "/api/v1/fs/info",
            .method = HTTP_GET,
            .handler = metadata_handler,
            .user_ctx = http,
        },
        {
            .uri = "/api/v1/fs/files",
            .method = HTTP_GET,
            .handler = metadata_handler,
            .user_ctx = http,
        },
        {
            .uri = FILE_ROUTE,
            .method = HTTP_GET,
            .handler = file_handler,
            .user_ctx = http,
        },
        {
            .uri = FILE_ROUTE,
            .method = HTTP_HEAD,
            .handler = file_handler,
            .user_ctx = http,
        },
        {
            .uri = FILE_ROUTE,
            .method = HTTP_PUT,
            .handler = file_handler,
            .user_ctx = http,
        },
        {
            .uri = FILE_ROUTE,
            .method = HTTP_DELETE,
            .handler = file_handler,
            .user_ctx = http,
        },
        {
            .uri = DIRECTORY_ROUTE,
            .method = HTTP_POST,
            .handler = file_handler,
            .user_ctx = http,
        },
    };
    esp_err_t err = app_http_server_register(
        http->http_server, FS_HTTP_OWNER, routes,
        sizeof(routes) / sizeof(routes[0]));
    if (err != ESP_OK) {
        free(http);
        return err;
    }
    *out_http = http;
    return ESP_OK;
}

esp_err_t fs_http_stop(fs_http_t *http, TickType_t wait_ticks)
{
    if (!http) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&http->lock);
    http->stopping = true;
    portEXIT_CRITICAL(&http->lock);
    esp_err_t err = app_http_server_unregister_owner(http->http_server,
                                                      FS_HTTP_OWNER);
    if (err != ESP_OK) {
        return err;
    }
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&http->lock);
        uint32_t active = http->active_requests;
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

esp_err_t fs_http_start(const fs_http_config_t *config, fs_http_t **out_http)
{
    (void)config;
    (void)out_http;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t fs_http_stop(fs_http_t *http, TickType_t wait_ticks)
{
    (void)http;
    (void)wait_ticks;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
