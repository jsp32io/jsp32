#include "nvs_http.h"

#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_partition.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "sdkconfig.h"

#define NVS_HTTP_OWNER "nvs_http"
#define API_ROOT "/api/v1/nvs"
#define NAMESPACE_ROOT API_ROOT "/namespaces"
#define MAX_PAGE_LIMIT 100
#define MAX_QUERY_SIZE 256

#if CONFIG_NVS_HTTP_ENABLED

struct nvs_http {
    app_http_server_t *http_server;
    app_nvs_t *store;
    portMUX_TYPE lock;
    uint32_t active_requests;
    bool stopping;
};

typedef struct {
    char namespace_name[NVS_NS_NAME_MAX_SIZE];
    char key[NVS_KEY_NAME_MAX_SIZE];
    enum { PATH_NAMESPACE, PATH_ENTRIES, PATH_ENTRY, PATH_BATCH } kind;
} parsed_path_t;

static esp_err_t send_json(httpd_req_t *request, const char *status,
                           cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) {
        httpd_resp_set_status(request, HTTPD_500);
        httpd_resp_set_type(request, "application/json");
        return httpd_resp_sendstr(request,
            "{\"ok\":false,\"error\":{\"code\":\"NVS_NO_MEMORY\","
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

static esp_err_t send_store_error(httpd_req_t *request, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return send_error(request, HTTPD_400, "NVS_INVALID_REQUEST",
                          "ValidationError", "invalid NVS request");
    }
    if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_NVS_NOT_FOUND ||
        err == APP_NVS_ERR_HIDDEN) {
        return send_error(request, HTTPD_404, "NVS_NOT_FOUND",
                          "NotFoundError", "namespace or entry not found");
    }
    if (err == APP_NVS_ERR_MANAGED) {
        return send_error(request, "403 Forbidden", "NVS_MANAGED",
                          "ForbiddenError", "resource is managed by firmware");
    }
    if (err == APP_NVS_ERR_PRECONDITION) {
        return send_error(request, "412 Precondition Failed",
                          "NVS_PRECONDITION_FAILED", "PreconditionError",
                          "entry precondition failed");
    }
    if (err == ESP_ERR_NVS_NOT_ENOUGH_SPACE) {
        return send_error(request, "507 Insufficient Storage", "NVS_FULL",
                          "StorageFullError", "NVS partition is full");
    }
    if (err == ESP_ERR_NVS_NOT_INITIALIZED) {
        return send_error(request, "503 Service Unavailable",
                          "NVS_UNAVAILABLE", "ServiceUnavailableError",
                          "NVS is not initialized");
    }
    if (err == ESP_ERR_NVS_TYPE_MISMATCH || err == ESP_ERR_NVS_VALUE_TOO_LONG) {
        return send_error(request, "422 Unprocessable Content",
                          "NVS_INVALID_VALUE", "ValidationError",
                          "value cannot be stored with the requested type");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_error(request, HTTPD_500, "NVS_NO_MEMORY",
                          "OutOfMemoryError", "not enough working memory");
    }
    return send_error(request, HTTPD_500, "NVS_ERROR", "NvsError",
                      "NVS operation failed");
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    value = (char)tolower((unsigned char)value);
    return value >= 'a' && value <= 'f' ? value - 'a' + 10 : -1;
}

static bool url_decode_segment(const char *input, size_t length, char *output,
                               size_t output_size)
{
    size_t written = 0;
    for (size_t i = 0; i < length; ++i) {
        unsigned char value = (unsigned char)input[i];
        if (value == '%') {
            if (i + 2 >= length) return false;
            int high = hex_value(input[i + 1]);
            int low = hex_value(input[i + 2]);
            if (high < 0 || low < 0) return false;
            value = (unsigned char)((high << 4) | low);
            i += 2;
        }
        if (value == 0 || value == '/' || value == '\\' || value < 0x20 ||
            value == 0x7f || written + 1 >= output_size) {
            return false;
        }
        output[written++] = (char)value;
    }
    if (written == 0) return false;
    output[written] = '\0';
    return true;
}

static bool parse_path(const char *uri, parsed_path_t *path)
{
    static const char prefix[] = NAMESPACE_ROOT "/";
    size_t uri_length = strcspn(uri, "?");
    if (uri_length <= sizeof(prefix) - 1 ||
        strncmp(uri, prefix, sizeof(prefix) - 1) != 0) {
        return false;
    }
    const char *parts[3] = {0};
    size_t lengths[3] = {0};
    size_t count = 0;
    const char *start = uri + sizeof(prefix) - 1;
    const char *end = uri + uri_length;
    while (start < end && count < 3) {
        const char *slash = memchr(start, '/', (size_t)(end - start));
        const char *stop = slash ? slash : end;
        if (stop == start) return false;
        parts[count] = start;
        lengths[count++] = (size_t)(stop - start);
        start = slash ? slash + 1 : end;
    }
    if (start < end || count == 0 ||
        !url_decode_segment(parts[0], lengths[0], path->namespace_name,
                            sizeof(path->namespace_name))) {
        return false;
    }
    if (count == 1) {
        path->kind = PATH_NAMESPACE;
        return true;
    }
    if (lengths[1] == 7 && memcmp(parts[1], "entries", 7) == 0) {
        if (count == 2) {
            path->kind = PATH_ENTRIES;
            return true;
        }
        if (count == 3 && url_decode_segment(parts[2], lengths[2], path->key,
                                             sizeof(path->key))) {
            path->kind = PATH_ENTRY;
            return true;
        }
        return false;
    }
    if (count == 2 && lengths[1] == 5 &&
        memcmp(parts[1], "batch", 5) == 0) {
        path->kind = PATH_BATCH;
        return true;
    }
    return false;
}

static bool get_header(httpd_req_t *request, const char *name, char *output,
                       size_t output_size)
{
    size_t length = httpd_req_get_hdr_value_len(request, name);
    return length > 0 && length < output_size &&
           httpd_req_get_hdr_value_str(request, name, output, output_size) ==
               ESP_OK;
}

static bool header_present(httpd_req_t *request, const char *name)
{
    return httpd_req_get_hdr_value_len(request, name) > 0;
}

static bool sha256_is_valid(const char *value)
{
    if (!value || strlen(value) != 64) return false;
    for (size_t i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)value[i])) return false;
    }
    return true;
}

static bool parse_etag(const char *header,
                       char output[APP_NVS_ETAG_HEX_SIZE])
{
    static const char prefix[] = "\"sha256-";
    size_t length = strlen(header);
    if (length != sizeof(prefix) - 1 + 64 + 1 ||
        strncmp(header, prefix, sizeof(prefix) - 1) != 0 ||
        header[length - 1] != '"') {
        return false;
    }
    memcpy(output, header + sizeof(prefix) - 1, 64);
    output[64] = '\0';
    return sha256_is_valid(output);
}

static void set_etag(httpd_req_t *request, const char *hash,
                     char header[80])
{
    snprintf(header, 80, "\"sha256-%s\"", hash);
    httpd_resp_set_hdr(request, "ETag", header);
}

static bool parse_pagination(httpd_req_t *request, size_t *limit, char *prefix,
                             size_t prefix_size, char *after,
                             size_t after_size)
{
    *limit = 50;
    prefix[0] = '\0';
    after[0] = '\0';
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0) return true;
    if (query_length > MAX_QUERY_SIZE) return false;
    char query[MAX_QUERY_SIZE + 1];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    char value[32];
    esp_err_t err = httpd_query_key_value(query, "limit", value,
                                          sizeof(value));
    if (err == ESP_OK) {
        char *tail = NULL;
        errno = 0;
        unsigned long parsed = strtoul(value, &tail, 10);
        if (errno || !tail || *tail || parsed == 0 || parsed > MAX_PAGE_LIMIT) {
            return false;
        }
        *limit = parsed;
    } else if (err != ESP_ERR_NOT_FOUND) {
        return false;
    }
    char encoded[CONFIG_HTTPD_MAX_URI_LEN + 1];
    err = httpd_query_key_value(query, "after", encoded, sizeof(encoded));
    if (err == ESP_OK) {
        if (!url_decode_segment(encoded, strlen(encoded), after, after_size)) {
            return false;
        }
    } else if (err != ESP_ERR_NOT_FOUND) {
        return false;
    }
    err = httpd_query_key_value(query, "prefix", encoded, sizeof(encoded));
    if (err == ESP_OK) {
        return url_decode_segment(encoded, strlen(encoded), prefix,
                                  prefix_size);
    }
    return err == ESP_ERR_NOT_FOUND;
}

static const char *policy_name(app_nvs_policy_t policy)
{
    return policy == APP_NVS_POLICY_USER ? "user" : "managed";
}

static cJSON *namespace_json(const app_nvs_namespace_info_t *info)
{
    cJSON *item = cJSON_CreateObject();
    if (item) {
        cJSON_AddStringToObject(item, "name", info->name);
        cJSON_AddStringToObject(item, "policy", policy_name(info->policy));
        cJSON_AddNumberToObject(item, "usedEntries", info->used_entries);
        cJSON_AddBoolToObject(item, "writable",
                              info->policy != APP_NVS_POLICY_HIDDEN);
    }
    return item;
}

static bool number_is_integer(const cJSON *json, double minimum,
                              double maximum, double *output)
{
    if (!cJSON_IsNumber(json) || !isfinite(json->valuedouble) ||
        floor(json->valuedouble) != json->valuedouble ||
        json->valuedouble < minimum || json->valuedouble > maximum) {
        return false;
    }
    *output = json->valuedouble;
    return true;
}

static bool parse_i64_string(const cJSON *json, int64_t *output)
{
    if (!cJSON_IsString(json) || !json->valuestring ||
        (!isdigit((unsigned char)json->valuestring[0]) &&
         !(json->valuestring[0] == '-' &&
           isdigit((unsigned char)json->valuestring[1])))) {
        return false;
    }
    char *tail = NULL;
    errno = 0;
    long long value = strtoll(json->valuestring, &tail, 10);
    if (errno || !tail || *tail) return false;
    *output = (int64_t)value;
    return true;
}

static bool parse_u64_string(const cJSON *json, uint64_t *output)
{
    if (!cJSON_IsString(json) || !json->valuestring ||
        !isdigit((unsigned char)json->valuestring[0])) {
        return false;
    }
    char *tail = NULL;
    errno = 0;
    unsigned long long value = strtoull(json->valuestring, &tail, 10);
    if (errno || !tail || *tail) return false;
    *output = (uint64_t)value;
    return true;
}

static nvs_type_t parse_type(const char *name)
{
    if (!name) return NVS_TYPE_ANY;
    if (strcmp(name, "i8") == 0) return NVS_TYPE_I8;
    if (strcmp(name, "u8") == 0) return NVS_TYPE_U8;
    if (strcmp(name, "i16") == 0) return NVS_TYPE_I16;
    if (strcmp(name, "u16") == 0) return NVS_TYPE_U16;
    if (strcmp(name, "i32") == 0) return NVS_TYPE_I32;
    if (strcmp(name, "u32") == 0) return NVS_TYPE_U32;
    if (strcmp(name, "i64") == 0) return NVS_TYPE_I64;
    if (strcmp(name, "u64") == 0) return NVS_TYPE_U64;
    if (strcmp(name, "float") == 0) return NVS_TYPE_FLOAT;
    if (strcmp(name, "double") == 0) return NVS_TYPE_DOUBLE;
    if (strcmp(name, "string") == 0) return NVS_TYPE_STR;
    if (strcmp(name, "blob") == 0) return NVS_TYPE_BLOB;
    return NVS_TYPE_ANY;
}

static esp_err_t parse_value(cJSON *object, app_nvs_value_t *output)
{
    memset(output, 0, sizeof(*output));
    cJSON *type_json = cJSON_GetObjectItemCaseSensitive(object, "type");
    cJSON *value = cJSON_GetObjectItemCaseSensitive(object, "value");
    if (!cJSON_IsString(type_json) || !value) return ESP_ERR_INVALID_ARG;
    output->type = parse_type(type_json->valuestring);
    double number = 0;
    switch (output->type) {
    case NVS_TYPE_I8:
        if (!number_is_integer(value, INT8_MIN, INT8_MAX, &number)) break;
        output->data.i8 = (int8_t)number; return ESP_OK;
    case NVS_TYPE_U8:
        if (!number_is_integer(value, 0, UINT8_MAX, &number)) break;
        output->data.u8 = (uint8_t)number; return ESP_OK;
    case NVS_TYPE_I16:
        if (!number_is_integer(value, INT16_MIN, INT16_MAX, &number)) break;
        output->data.i16 = (int16_t)number; return ESP_OK;
    case NVS_TYPE_U16:
        if (!number_is_integer(value, 0, UINT16_MAX, &number)) break;
        output->data.u16 = (uint16_t)number; return ESP_OK;
    case NVS_TYPE_I32:
        if (!number_is_integer(value, INT32_MIN, INT32_MAX, &number)) break;
        output->data.i32 = (int32_t)number; return ESP_OK;
    case NVS_TYPE_U32:
        if (!number_is_integer(value, 0, UINT32_MAX, &number)) break;
        output->data.u32 = (uint32_t)number; return ESP_OK;
    case NVS_TYPE_I64:
        return parse_i64_string(value, &output->data.i64)
                   ? ESP_OK : ESP_ERR_INVALID_ARG;
    case NVS_TYPE_U64:
        return parse_u64_string(value, &output->data.u64)
                   ? ESP_OK : ESP_ERR_INVALID_ARG;
    case NVS_TYPE_FLOAT:
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
            fabs(value->valuedouble) > FLT_MAX) break;
        output->data.f32 = (float)value->valuedouble; return ESP_OK;
    case NVS_TYPE_DOUBLE:
        if (!cJSON_IsNumber(value) || !isfinite(value->valuedouble)) break;
        output->data.f64 = value->valuedouble; return ESP_OK;
    case NVS_TYPE_STR: {
        if (!cJSON_IsString(value) || !value->valuestring) break;
        size_t size = strlen(value->valuestring);
        if (size > CONFIG_NVS_HTTP_MAX_STRING_SIZE) return ESP_ERR_INVALID_SIZE;
        char *copy = malloc(size + 1);
        if (!copy) return ESP_ERR_NO_MEM;
        memcpy(copy, value->valuestring, size + 1);
        output->data.bytes.data = copy;
        output->data.bytes.size = size;
        return ESP_OK;
    }
    case NVS_TYPE_BLOB: {
        cJSON *encoding = cJSON_GetObjectItemCaseSensitive(object, "encoding");
        if (!cJSON_IsString(value) || !value->valuestring ||
            (encoding && (!cJSON_IsString(encoding) ||
                          strcmp(encoding->valuestring, "base64") != 0))) {
            break;
        }
        size_t encoded_size = strlen(value->valuestring);
        size_t decoded_size = 0;
        int result = mbedtls_base64_decode(NULL, 0, &decoded_size,
                                           (const unsigned char *)
                                               value->valuestring,
                                           encoded_size);
        if (encoded_size == 0) decoded_size = 0;
        else if (result != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) break;
        if (decoded_size > CONFIG_NVS_HTTP_MAX_BLOB_SIZE) {
            return ESP_ERR_INVALID_SIZE;
        }
        unsigned char *data = malloc(decoded_size == 0 ? 1 : decoded_size);
        if (!data) return ESP_ERR_NO_MEM;
        if (decoded_size > 0 &&
            mbedtls_base64_decode(data, decoded_size, &decoded_size,
                                  (const unsigned char *)value->valuestring,
                                  encoded_size) != 0) {
            free(data);
            break;
        }
        output->data.bytes.data = data;
        output->data.bytes.size = decoded_size;
        return ESP_OK;
    }
    default:
        break;
    }
    memset(output, 0, sizeof(*output));
    return ESP_ERR_INVALID_ARG;
}

static bool add_value_json(cJSON *object, const app_nvs_value_t *value)
{
    cJSON_AddStringToObject(object, "type", app_nvs_type_name(value->type));
    char decimal[32];
    switch (value->type) {
    case NVS_TYPE_I8: cJSON_AddNumberToObject(object, "value", value->data.i8); break;
    case NVS_TYPE_U8: cJSON_AddNumberToObject(object, "value", value->data.u8); break;
    case NVS_TYPE_I16: cJSON_AddNumberToObject(object, "value", value->data.i16); break;
    case NVS_TYPE_U16: cJSON_AddNumberToObject(object, "value", value->data.u16); break;
    case NVS_TYPE_I32: cJSON_AddNumberToObject(object, "value", value->data.i32); break;
    case NVS_TYPE_U32: cJSON_AddNumberToObject(object, "value", value->data.u32); break;
    case NVS_TYPE_I64:
        snprintf(decimal, sizeof(decimal), "%lld", (long long)value->data.i64);
        cJSON_AddStringToObject(object, "value", decimal); break;
    case NVS_TYPE_U64:
        snprintf(decimal, sizeof(decimal), "%llu",
                 (unsigned long long)value->data.u64);
        cJSON_AddStringToObject(object, "value", decimal); break;
    case NVS_TYPE_FLOAT:
        if (!isfinite(value->data.f32)) return false;
        cJSON_AddNumberToObject(object, "value", value->data.f32); break;
    case NVS_TYPE_DOUBLE:
        if (!isfinite(value->data.f64)) return false;
        cJSON_AddNumberToObject(object, "value", value->data.f64); break;
    case NVS_TYPE_STR:
        cJSON_AddStringToObject(object, "value", value->data.bytes.data); break;
    case NVS_TYPE_BLOB: {
        size_t capacity = ((value->data.bytes.size + 2) / 3) * 4 + 1;
        unsigned char *encoded = malloc(capacity);
        size_t size = 0;
        if (!encoded || mbedtls_base64_encode(encoded, capacity, &size,
                value->data.bytes.data, value->data.bytes.size) != 0) {
            free(encoded);
            return false;
        }
        encoded[size] = '\0';
        cJSON_AddStringToObject(object, "encoding", "base64");
        cJSON_AddStringToObject(object, "value", (char *)encoded);
        free(encoded);
        break;
    }
    default:
        return false;
    }
    return true;
}

static cJSON *read_body_json(httpd_req_t *request, esp_err_t *error)
{
    *error = ESP_ERR_INVALID_ARG;
    if (request->content_len <= 0 ||
        request->content_len > CONFIG_NVS_HTTP_MAX_BODY_SIZE) {
        if (request->content_len > CONFIG_NVS_HTTP_MAX_BODY_SIZE) {
            *error = ESP_ERR_INVALID_SIZE;
        }
        return NULL;
    }
    char content_type[64];
    if (!get_header(request, "Content-Type", content_type,
                    sizeof(content_type)) ||
        strncasecmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';' &&
         !isspace((unsigned char)content_type[16]))) {
        *error = ESP_ERR_NOT_SUPPORTED;
        return NULL;
    }
    char *body = malloc((size_t)request->content_len + 1);
    if (!body) {
        *error = ESP_ERR_NO_MEM;
        return NULL;
    }
    size_t received = 0;
    while (received < (size_t)request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0) {
            free(body);
            *error = ESP_FAIL;
            return NULL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *json = cJSON_ParseWithLength(body, received);
    free(body);
    if (!cJSON_IsObject(json)) {
        cJSON_Delete(json);
        return NULL;
    }
    *error = ESP_OK;
    return json;
}

static esp_err_t send_body_error(httpd_req_t *request, esp_err_t err)
{
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_error(request, "413 Content Too Large",
                          "NVS_BODY_TOO_LARGE", "RequestTooLargeError",
                          "JSON body exceeds the configured limit");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_error(request, "415 Unsupported Media Type",
                          "NVS_MEDIA_TYPE", "UnsupportedMediaTypeError",
                          "Content-Type must be application/json");
    }
    if (err == ESP_ERR_NO_MEM) return send_store_error(request, err);
    return send_error(request, HTTPD_400, "NVS_INVALID_JSON",
                      "ValidationError", "request body must be a JSON object");
}

static esp_err_t info_handler(httpd_req_t *request, nvs_http_t *http)
{
    nvs_stats_t stats;
    esp_err_t err = app_nvs_stats(http->store, &stats);
    if (err != ESP_OK) return send_store_error(request, err);
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *limits = cJSON_CreateObject();
    if (!root || !data || !limits) {
        cJSON_Delete(root); cJSON_Delete(data); cJSON_Delete(limits);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "partition", "nvs");
    cJSON_AddNumberToObject(data, "size", partition ? partition->size : 0);
    cJSON_AddBoolToObject(data, "encrypted",
                          partition ? partition->encrypted : false);
    cJSON_AddNumberToObject(data, "usedEntries", stats.used_entries);
    cJSON_AddNumberToObject(data, "freeEntries", stats.free_entries);
    cJSON_AddNumberToObject(data, "totalEntries", stats.total_entries);
    cJSON_AddNumberToObject(data, "namespaceCount", stats.namespace_count);
    cJSON_AddItemToObject(data, "limits", limits);
    cJSON_AddNumberToObject(limits, "maxStringBytes",
                            CONFIG_NVS_HTTP_MAX_STRING_SIZE);
    cJSON_AddNumberToObject(limits, "maxBlobBytes",
                            CONFIG_NVS_HTTP_MAX_BLOB_SIZE);
    cJSON_AddNumberToObject(limits, "maxBatchOperations",
                            CONFIG_NVS_HTTP_MAX_BATCH_OPERATIONS);
    return send_json(request, HTTPD_200, root);
}

static esp_err_t namespaces_handler(httpd_req_t *request, nvs_http_t *http)
{
    size_t limit;
    char prefix[NVS_NS_NAME_MAX_SIZE];
    char after[NVS_NS_NAME_MAX_SIZE];
    if (!parse_pagination(request, &limit, prefix, sizeof(prefix), after,
                          sizeof(after))) {
        return send_error(request, HTTPD_400, "NVS_INVALID_QUERY",
                          "ValidationError", "invalid pagination query");
    }
    app_nvs_namespace_info_t *items = calloc(limit, sizeof(*items));
    if (!items) return send_store_error(request, ESP_ERR_NO_MEM);
    size_t count = 0;
    bool has_more = false;
    esp_err_t err = app_nvs_list_namespaces(http->store, prefix, after, items,
                                             limit, &count, &has_more);
    if (err != ESP_OK) {
        free(items);
        return send_store_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (!root || !data || !array) {
        cJSON_Delete(root); cJSON_Delete(data); cJSON_Delete(array); free(items);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddItemToObject(data, "namespaces", array);
    for (size_t i = 0; i < count; ++i) {
        cJSON_AddItemToArray(array, namespace_json(&items[i]));
    }
    cJSON_AddBoolToObject(data, "hasMore", has_more);
    if (has_more && count > 0) {
        cJSON_AddStringToObject(data, "nextAfter", items[count - 1].name);
    } else {
        cJSON_AddNullToObject(data, "nextAfter");
    }
    free(items);
    return send_json(request, HTTPD_200, root);
}

static esp_err_t namespace_get(httpd_req_t *request, nvs_http_t *http,
                               const char *namespace_name)
{
    app_nvs_namespace_info_t info;
    esp_err_t err = app_nvs_namespace_info(http->store, namespace_name, &info);
    if (err != ESP_OK) return send_store_error(request, err);
    cJSON *root = cJSON_CreateObject();
    cJSON *data = namespace_json(&info);
    if (!root || !data) {
        cJSON_Delete(root); cJSON_Delete(data);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    return send_json(request, HTTPD_200, root);
}

static esp_err_t namespace_delete(httpd_req_t *request, nvs_http_t *http,
                                  const char *namespace_name)
{
    char confirmation[NVS_NS_NAME_MAX_SIZE];
    if (!get_header(request, "X-Confirm-Namespace", confirmation,
                    sizeof(confirmation)) ||
        strcmp(confirmation, namespace_name) != 0) {
        return send_error(request, HTTPD_400, "NVS_CONFIRMATION_REQUIRED",
                          "ValidationError",
                          "X-Confirm-Namespace must match the namespace");
    }
    esp_err_t err = app_nvs_erase_namespace(http->store, namespace_name);
    if (err != ESP_OK) return send_store_error(request, err);
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static esp_err_t entries_handler(httpd_req_t *request, nvs_http_t *http,
                                 const char *namespace_name)
{
    size_t limit;
    char prefix[NVS_KEY_NAME_MAX_SIZE];
    char after[NVS_KEY_NAME_MAX_SIZE];
    if (!parse_pagination(request, &limit, prefix, sizeof(prefix), after,
                          sizeof(after))) {
        return send_error(request, HTTPD_400, "NVS_INVALID_QUERY",
                          "ValidationError", "invalid pagination query");
    }
    app_nvs_entry_info_t *items = calloc(limit, sizeof(*items));
    if (!items) return send_store_error(request, ESP_ERR_NO_MEM);
    size_t count = 0;
    bool has_more = false;
    esp_err_t err = app_nvs_list_entries(http->store, namespace_name, prefix,
                                         after, items, limit, &count,
                                         &has_more);
    if (err != ESP_OK) {
        free(items);
        return send_store_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *array = cJSON_CreateArray();
    if (!root || !data || !array) {
        cJSON_Delete(root); cJSON_Delete(data); cJSON_Delete(array); free(items);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "namespace", namespace_name);
    cJSON_AddItemToObject(data, "entries", array);
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", items[i].key);
        cJSON_AddStringToObject(item, "type", app_nvs_type_name(items[i].type));
        if (items[i].sensitive) cJSON_AddNullToObject(item, "size");
        else cJSON_AddNumberToObject(item, "size", items[i].size);
        cJSON_AddBoolToObject(item, "readable", items[i].readable);
        cJSON_AddBoolToObject(item, "writable", items[i].writable);
        cJSON_AddItemToArray(array, item);
    }
    cJSON_AddBoolToObject(data, "hasMore", has_more);
    if (has_more && count > 0) {
        cJSON_AddStringToObject(data, "nextAfter", items[count - 1].key);
    } else {
        cJSON_AddNullToObject(data, "nextAfter");
    }
    free(items);
    return send_json(request, HTTPD_200, root);
}

static cJSON *entry_json(const char *namespace_name, const char *key,
                         const app_nvs_value_t *value, const char *etag)
{
    cJSON *data = cJSON_CreateObject();
    if (!data) return NULL;
    cJSON_AddStringToObject(data, "namespace", namespace_name);
    cJSON_AddStringToObject(data, "key", key);
    if (!add_value_json(data, value)) {
        cJSON_Delete(data);
        return NULL;
    }
    char header[80];
    snprintf(header, sizeof(header), "\"sha256-%s\"", etag);
    cJSON_AddStringToObject(data, "etag", header);
    return data;
}

static esp_err_t entry_get(httpd_req_t *request, nvs_http_t *http,
                           const char *namespace_name, const char *key)
{
    app_nvs_value_t value;
    char etag[APP_NVS_ETAG_HEX_SIZE];
    char etag_header[80];
    esp_err_t err = app_nvs_get(http->store, namespace_name, key, &value, etag);
    if (err != ESP_OK) return send_store_error(request, err);
    if (header_present(request, "If-None-Match")) {
        char condition[80], expected[APP_NVS_ETAG_HEX_SIZE];
        if (!get_header(request, "If-None-Match", condition,
                        sizeof(condition))) {
            app_nvs_value_free(&value);
            return send_error(request, HTTPD_400, "NVS_INVALID_ETAG",
                              "ValidationError", "invalid If-None-Match");
        }
        bool matches = strcmp(condition, "*") == 0 ||
                       (parse_etag(condition, expected) &&
                        strcasecmp(expected, etag) == 0);
        if (matches) {
            set_etag(request, etag, etag_header);
            app_nvs_value_free(&value);
            httpd_resp_set_status(request, "304 Not Modified");
            return httpd_resp_send(request, NULL, 0);
        }
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = entry_json(namespace_name, key, &value, etag);
    app_nvs_value_free(&value);
    if (!root || !data) {
        cJSON_Delete(root); cJSON_Delete(data);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    set_etag(request, etag, etag_header);
    return send_json(request, HTTPD_200, root);
}

static bool request_condition(httpd_req_t *request,
                              app_nvs_condition_t *condition,
                              char expected[APP_NVS_ETAG_HEX_SIZE])
{
    bool has_match = header_present(request, "If-Match");
    bool has_none = header_present(request, "If-None-Match");
    if (has_match && has_none) return false;
    *condition = APP_NVS_CONDITION_UPSERT;
    expected[0] = '\0';
    char header[80];
    if (has_match) {
        if (!get_header(request, "If-Match", header, sizeof(header)) ||
            !parse_etag(header, expected)) return false;
        *condition = APP_NVS_CONDITION_MATCH;
    } else if (has_none) {
        if (!get_header(request, "If-None-Match", header, sizeof(header)) ||
            strcmp(header, "*") != 0) return false;
        *condition = APP_NVS_CONDITION_CREATE_ONLY;
    }
    return true;
}

static esp_err_t entry_put(httpd_req_t *request, nvs_http_t *http,
                           const char *namespace_name, const char *key)
{
    app_nvs_condition_t condition;
    char expected[APP_NVS_ETAG_HEX_SIZE];
    if (!request_condition(request, &condition, expected)) {
        return send_error(request, HTTPD_400, "NVS_INVALID_ETAG",
                          "ValidationError", "invalid conditional header");
    }
    esp_err_t err;
    cJSON *json = read_body_json(request, &err);
    if (!json) return send_body_error(request, err);
    app_nvs_value_t value;
    err = parse_value(json, &value);
    cJSON_Delete(json);
    if (err != ESP_OK) return send_store_error(request, err);
    bool created = false;
    char etag[APP_NVS_ETAG_HEX_SIZE];
    char etag_header[80];
    err = app_nvs_set(http->store, namespace_name, key, &value, condition,
                      expected[0] ? expected : NULL, &created, etag);
    if (err != ESP_OK) {
        app_nvs_value_free(&value);
        return send_store_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = entry_json(namespace_name, key, &value, etag);
    app_nvs_value_free(&value);
    if (!root || !data) {
        cJSON_Delete(root); cJSON_Delete(data);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    set_etag(request, etag, etag_header);
    return send_json(request, created ? "201 Created" : HTTPD_200, root);
}

static esp_err_t entry_delete(httpd_req_t *request, nvs_http_t *http,
                              const char *namespace_name, const char *key)
{
    char expected[APP_NVS_ETAG_HEX_SIZE] = {0};
    if (header_present(request, "If-Match")) {
        char header[80];
        if (!get_header(request, "If-Match", header, sizeof(header)) ||
            !parse_etag(header, expected)) {
            return send_error(request, HTTPD_400, "NVS_INVALID_ETAG",
                              "ValidationError", "invalid If-Match");
        }
    }
    esp_err_t err = app_nvs_erase(http->store, namespace_name, key,
                                  expected[0] ? expected : NULL);
    if (err != ESP_OK) return send_store_error(request, err);
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

static void free_batch_values(app_nvs_value_t *values, size_t count)
{
    if (!values) return;
    for (size_t i = 0; i < count; ++i) app_nvs_value_free(&values[i]);
    free(values);
}

static bool batch_condition(cJSON *item, bool erase,
                            app_nvs_batch_operation_t *operation,
                            char expected[APP_NVS_ETAG_HEX_SIZE])
{
    cJSON *match = cJSON_GetObjectItemCaseSensitive(item, "ifMatch");
    cJSON *none = cJSON_GetObjectItemCaseSensitive(item, "ifNoneMatch");
    if (match && none) return false;
    operation->condition = APP_NVS_CONDITION_UPSERT;
    if (match) {
        if (!cJSON_IsString(match) || !match->valuestring ||
            !parse_etag(match->valuestring, expected)) return false;
        operation->condition = APP_NVS_CONDITION_MATCH;
        operation->expected_etag = expected;
    } else if (none) {
        if (erase || !cJSON_IsTrue(none)) return false;
        operation->condition = APP_NVS_CONDITION_CREATE_ONLY;
    }
    return true;
}

static esp_err_t batch_handler(httpd_req_t *request, nvs_http_t *http,
                               const char *namespace_name)
{
    esp_err_t err;
    cJSON *json = read_body_json(request, &err);
    if (!json) return send_body_error(request, err);
    cJSON *array = cJSON_GetObjectItemCaseSensitive(json, "operations");
    int count_int = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0;
    if (count_int <= 0 || count_int > CONFIG_NVS_HTTP_MAX_BATCH_OPERATIONS) {
        cJSON_Delete(json);
        return send_error(request, HTTPD_400, "NVS_INVALID_BATCH",
                          "ValidationError", "invalid batch operations");
    }
    size_t count = (size_t)count_int;
    app_nvs_batch_operation_t *operations = calloc(count, sizeof(*operations));
    app_nvs_value_t *values = calloc(count, sizeof(*values));
    app_nvs_batch_result_t *results = calloc(count, sizeof(*results));
    char (*keys)[NVS_KEY_NAME_MAX_SIZE] = calloc(count, sizeof(*keys));
    char (*etags)[APP_NVS_ETAG_HEX_SIZE] = calloc(count, sizeof(*etags));
    if (!operations || !values || !results || !keys || !etags) {
        free(operations); free_batch_values(values, count); free(results);
        free(keys); free(etags); cJSON_Delete(json);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    err = ESP_OK;
    for (size_t i = 0; i < count && err == ESP_OK; ++i) {
        cJSON *item = cJSON_GetArrayItem(array, (int)i);
        cJSON *op = cJSON_GetObjectItemCaseSensitive(item, "op");
        cJSON *key = cJSON_GetObjectItemCaseSensitive(item, "key");
        if (!cJSON_IsObject(item) || !cJSON_IsString(op) ||
            !cJSON_IsString(key) || !key->valuestring ||
            strlcpy(keys[i], key->valuestring, sizeof(keys[i])) >=
                sizeof(keys[i])) {
            err = ESP_ERR_INVALID_ARG;
            break;
        }
        operations[i].key = keys[i];
        if (strcmp(op->valuestring, "set") == 0) {
            err = parse_value(item, &values[i]);
            operations[i].value = &values[i];
        } else if (strcmp(op->valuestring, "delete") == 0) {
            operations[i].erase = true;
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
        if (err == ESP_OK &&
            !batch_condition(item, operations[i].erase, &operations[i],
                             etags[i])) {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    if (err == ESP_OK) {
        err = app_nvs_batch(http->store, namespace_name, operations, count,
                            results);
    }
    cJSON_Delete(json);
    if (err != ESP_OK) {
        free(operations); free_batch_values(values, count); free(results);
        free(keys); free(etags);
        return send_store_error(request, err);
    }
    cJSON *root = cJSON_CreateObject();
    cJSON *data = cJSON_CreateObject();
    cJSON *response = cJSON_CreateArray();
    if (!root || !data || !response) {
        cJSON_Delete(root); cJSON_Delete(data); cJSON_Delete(response);
        free(operations); free_batch_values(values, count); free(results);
        free(keys); free(etags);
        return send_store_error(request, ESP_ERR_NO_MEM);
    }
    cJSON_AddTrueToObject(root, "ok");
    cJSON_AddItemToObject(root, "data", data);
    cJSON_AddStringToObject(data, "namespace", namespace_name);
    cJSON_AddItemToObject(data, "operations", response);
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "op",
                                operations[i].erase ? "delete" : "set");
        cJSON_AddStringToObject(item, "key", operations[i].key);
        if (operations[i].erase) {
            cJSON_AddTrueToObject(item, "deleted");
        } else {
            char header[80];
            snprintf(header, sizeof(header), "\"sha256-%s\"",
                     results[i].etag);
            cJSON_AddBoolToObject(item, "created", results[i].created);
            cJSON_AddStringToObject(item, "etag", header);
        }
        cJSON_AddItemToArray(response, item);
    }
    free(operations); free_batch_values(values, count); free(results);
    free(keys); free(etags);
    return send_json(request, HTTPD_200, root);
}

static esp_err_t dispatch(httpd_req_t *request, nvs_http_t *http)
{
    size_t uri_length = strcspn(request->uri, "?");
    if (uri_length == strlen(API_ROOT) &&
        strncmp(request->uri, API_ROOT, uri_length) == 0) {
        return info_handler(request, http);
    }
    if (uri_length == strlen(NAMESPACE_ROOT) &&
        strncmp(request->uri, NAMESPACE_ROOT, uri_length) == 0) {
        return namespaces_handler(request, http);
    }
    parsed_path_t path = {0};
    if (!parse_path(request->uri, &path)) {
        return send_error(request, HTTPD_404, "NVS_NOT_FOUND",
                          "NotFoundError", "NVS API route not found");
    }
    if (path.kind == PATH_NAMESPACE) {
        if (request->method == HTTP_GET) {
            return namespace_get(request, http, path.namespace_name);
        }
        if (request->method == HTTP_DELETE) {
            return namespace_delete(request, http, path.namespace_name);
        }
    } else if (path.kind == PATH_ENTRIES && request->method == HTTP_GET) {
        return entries_handler(request, http, path.namespace_name);
    } else if (path.kind == PATH_ENTRY) {
        if (request->method == HTTP_GET) {
            return entry_get(request, http, path.namespace_name, path.key);
        }
        if (request->method == HTTP_PUT) {
            return entry_put(request, http, path.namespace_name, path.key);
        }
        if (request->method == HTTP_DELETE) {
            return entry_delete(request, http, path.namespace_name, path.key);
        }
    } else if (path.kind == PATH_BATCH && request->method == HTTP_POST) {
        return batch_handler(request, http, path.namespace_name);
    }
    return send_error(request, "405 Method Not Allowed",
                      "NVS_METHOD_NOT_ALLOWED", "MethodNotAllowedError",
                      "method is not allowed for this resource");
}

static bool request_try_acquire(nvs_http_t *http)
{
    if (!app_http_server_worker_try_acquire(http->http_server)) return false;
    bool acquired = false;
    portENTER_CRITICAL(&http->lock);
    if (!http->stopping &&
        http->active_requests < CONFIG_NVS_HTTP_MAX_CONCURRENT_REQUESTS) {
        ++http->active_requests;
        acquired = true;
    }
    portEXIT_CRITICAL(&http->lock);
    if (!acquired) app_http_server_worker_release(http->http_server);
    return acquired;
}

static void request_release(nvs_http_t *http)
{
    portENTER_CRITICAL(&http->lock);
    if (http->active_requests > 0) --http->active_requests;
    portEXIT_CRITICAL(&http->lock);
    app_http_server_worker_release(http->http_server);
}

static void request_task(void *argument)
{
    httpd_req_t *request = argument;
    nvs_http_t *http = request->user_ctx;
    dispatch(request, http);
    httpd_req_async_handler_complete(request);
    request_release(http);
    vTaskDelete(NULL);
}

static esp_err_t route_handler(httpd_req_t *request)
{
    nvs_http_t *http = request->user_ctx;
    if (request->content_len > CONFIG_NVS_HTTP_MAX_BODY_SIZE) {
        return send_body_error(request, ESP_ERR_INVALID_SIZE);
    }
    if (!request_try_acquire(http)) {
        return send_error(request, "429 Too Many Requests", "NVS_BUSY",
                          "BusyError", "too many NVS requests");
    }
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        request_release(http);
        return err;
    }
    if (xTaskCreate(request_task, "nvs_http", CONFIG_NVS_HTTP_TASK_STACK_SIZE,
                    async_request, CONFIG_NVS_HTTP_TASK_PRIORITY, NULL) !=
        pdPASS) {
        httpd_req_async_handler_complete(async_request);
        request_release(http);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t nvs_http_start(const nvs_http_config_t *config,
                         nvs_http_t **out_http)
{
    if (!config || !config->http_server || !config->store || !out_http) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_http_t *http = calloc(1, sizeof(*http));
    if (!http) return ESP_ERR_NO_MEM;
    http->http_server = config->http_server;
    http->store = config->store;
    http->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    const httpd_uri_t routes[] = {
        {.uri = API_ROOT, .method = HTTP_GET, .handler = route_handler,
         .user_ctx = http},
        {.uri = NAMESPACE_ROOT, .method = HTTP_GET, .handler = route_handler,
         .user_ctx = http},
        {.uri = NAMESPACE_ROOT "/*", .method = HTTP_GET,
         .handler = route_handler, .user_ctx = http},
        {.uri = NAMESPACE_ROOT "/*", .method = HTTP_PUT,
         .handler = route_handler, .user_ctx = http},
        {.uri = NAMESPACE_ROOT "/*", .method = HTTP_DELETE,
         .handler = route_handler, .user_ctx = http},
        {.uri = NAMESPACE_ROOT "/*", .method = HTTP_POST,
         .handler = route_handler, .user_ctx = http},
    };
    esp_err_t err = app_http_server_register(http->http_server, NVS_HTTP_OWNER,
        routes, sizeof(routes) / sizeof(routes[0]));
    if (err != ESP_OK) {
        free(http);
        return err;
    }
    *out_http = http;
    return ESP_OK;
}

esp_err_t nvs_http_stop(nvs_http_t *http, TickType_t wait_ticks)
{
    if (!http) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&http->lock);
    http->stopping = true;
    portEXIT_CRITICAL(&http->lock);
    esp_err_t err = app_http_server_unregister_owner(http->http_server,
                                                      NVS_HTTP_OWNER);
    if (err != ESP_OK) return err;
    TickType_t started = xTaskGetTickCount();
    for (;;) {
        portENTER_CRITICAL(&http->lock);
        uint32_t active = http->active_requests;
        portEXIT_CRITICAL(&http->lock);
        if (active == 0) {
            free(http);
            return ESP_OK;
        }
        if (wait_ticks == 0 || xTaskGetTickCount() - started >= wait_ticks) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
}

#else

esp_err_t nvs_http_start(const nvs_http_config_t *config,
                         nvs_http_t **out_http)
{
    (void)config; (void)out_http;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t nvs_http_stop(nvs_http_t *http, TickType_t wait_ticks)
{
    (void)http; (void)wait_ticks;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
