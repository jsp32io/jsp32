#include "reg_http.h"
#include "reg_http_filter.h"
#include "reg_http_connection.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_REG_HTTP_ENABLED

#define REG_HTTP_OWNER "reg_http"
#define REG_ROOT "/api/v1/registers"
#define REG_BODY_LIMIT 32768
#define REG_EVENT_RING_SIZE 128

typedef struct {
    uint64_t revision;
    app_register_source_t source;
    app_register_change_t change;
} reg_change_t;

typedef struct {
    httpd_req_t *request;
    reg_http_filter_t filter;
} reg_event_client_t;

struct reg_http {
    app_http_server_t *http_server;
    app_registers_t *registers;
    portMUX_TYPE lock;
    uint64_t next_sequence;
    reg_change_t ring[REG_EVENT_RING_SIZE];
    uint32_t active_clients;
    uint32_t subscriber_id;
};

static esp_err_t send_json(httpd_req_t *request, const char *status,
                           cJSON *root)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_set_status(request, HTTPD_500);
        return httpd_resp_sendstr(request, "Out of memory");
    }
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, strlen(body));
    cJSON_free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, const char *status,
                            const char *code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *error = root ? cJSON_AddObjectToObject(root, "error") : NULL;
    if (!root || !error) {
        cJSON_Delete(root);
        httpd_resp_set_status(request, HTTPD_500);
        return httpd_resp_sendstr(request, "Out of memory");
    }
    cJSON_AddFalseToObject(root, "ok");
    cJSON_AddStringToObject(error, "code", code);
    cJSON_AddStringToObject(error, "name", "RegisterError");
    cJSON_AddStringToObject(error, "message", message);
    return send_json(request, status, root);
}

static void add_revision(cJSON *root, uint64_t revision)
{
    char text[24];
    snprintf(text, sizeof(text), "%" PRIu64, revision);
    cJSON_AddStringToObject(root, "revision", text);
}

static bool parse_address(const cJSON *item, app_register_address_t *address)
{
    return cJSON_IsString(item) && item->valuestring &&
           app_register_address_parse(item->valuestring, address) == ESP_OK;
}

static bool parse_count(const cJSON *item, size_t *count)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 1 ||
        item->valuedouble > CONFIG_APP_REGISTERS_MAX_TRANSACTION ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *count = (size_t)item->valuedouble;
    return true;
}

static bool parse_value(const cJSON *item, app_register_address_t address,
                        uint16_t *value)
{
    if (app_register_area_is_bit(address.area)) {
        if (cJSON_IsBool(item)) {
            *value = cJSON_IsTrue(item) ? 1 : 0;
            return true;
        }
    }
    double maximum = app_register_area_is_bit(address.area) ? 1 : UINT16_MAX;
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < 0 || item->valuedouble > maximum ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *value = (uint16_t)item->valuedouble;
    return true;
}

static cJSON *read_body(httpd_req_t *request)
{
    if (request->content_len == 0 || request->content_len > REG_BODY_LIMIT) {
        return NULL;
    }
    char *body = malloc(request->content_len + 1);
    if (!body) return NULL;
    size_t received = 0;
    unsigned timeouts = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
        if (count <= 0) {
            free(body);
            return NULL;
        }
        timeouts = 0;
        received += (size_t)count;
    }
    body[received] = '\0';
    cJSON *json = cJSON_ParseWithLength(body, received);
    free(body);
    return json;
}

static esp_err_t info_handler(httpd_req_t *request)
{
    reg_http_t *http = request->user_ctx;
    cJSON *root = cJSON_CreateObject();
    cJSON *areas = root ? cJSON_AddObjectToObject(root, "areas") : NULL;
    if (!root || !areas) {
        cJSON_Delete(root);
        return send_error(request, HTTPD_500, "REG_NO_MEMORY", "out of memory");
    }
    cJSON_AddTrueToObject(root, "ok");
    add_revision(root, app_registers_revision(http->registers));
    cJSON_AddNumberToObject(areas, "X", app_registers_area_count(APP_REGISTER_AREA_X));
    cJSON_AddNumberToObject(areas, "Y", app_registers_area_count(APP_REGISTER_AREA_Y));
    cJSON_AddNumberToObject(areas, "M", app_registers_area_count(APP_REGISTER_AREA_M));
    cJSON_AddNumberToObject(areas, "D", app_registers_area_count(APP_REGISTER_AREA_D));
    cJSON_AddNumberToObject(root, "maxTransaction",
                            CONFIG_APP_REGISTERS_MAX_TRANSACTION);
    cJSON_AddTrueToObject(root, "volatile");
    return send_json(request, HTTPD_200, root);
}

static esp_err_t read_handler(httpd_req_t *request)
{
    reg_http_t *http = request->user_ctx;
    cJSON *body = read_body(request);
    if (!body) return send_error(request, HTTPD_400, "REG_INVALID_REQUEST",
                                 "invalid or oversized JSON body");
    cJSON *list = cJSON_GetObjectItemCaseSensitive(body, "addresses");
    cJSON *start = cJSON_GetObjectItemCaseSensitive(body, "start");
    cJSON *count_json = cJSON_GetObjectItemCaseSensitive(body, "count");
    size_t count = 0;
    app_register_address_t first = {0};
    bool range = !list && parse_address(start, &first) &&
                 parse_count(count_json, &count) &&
                 count <= app_registers_area_count(first.area) - first.index;
    if (list && !start && !count_json && cJSON_IsArray(list)) {
        count = cJSON_GetArraySize(list);
    }
    if ((!range && !list) || count == 0 ||
        count > CONFIG_APP_REGISTERS_MAX_TRANSACTION) {
        cJSON_Delete(body);
        return send_error(request, HTTPD_400, "REG_INVALID_REQUEST",
                          "invalid register read request");
    }
    app_register_address_t *addresses = calloc(count, sizeof(*addresses));
    uint16_t *values = calloc(count, sizeof(*values));
    if (!addresses || !values) {
        free(addresses);
        free(values);
        cJSON_Delete(body);
        return send_error(request, HTTPD_500, "REG_NO_MEMORY", "out of memory");
    }
    bool valid = range;
    for (size_t i = 0; i < count; ++i) {
        if (range) {
            addresses[i] = (app_register_address_t){first.area,
                                                       (uint16_t)(first.index + i)};
        } else if (!parse_address(cJSON_GetArrayItem(list, i), &addresses[i])) {
            valid = false;
            break;
        } else {
            valid = true;
        }
    }
    uint64_t revision = 0;
    esp_err_t err = valid ? app_registers_read_many(http->registers, addresses,
                                                    values, count, &revision)
                          : ESP_ERR_INVALID_ARG;
    cJSON *root = NULL;
    cJSON *results = NULL;
    if (err == ESP_OK) {
        root = cJSON_CreateObject();
        results = root ? cJSON_AddArrayToObject(root, "values") : NULL;
        if (!root || !results) err = ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; err == ESP_OK && i < count; ++i) {
        char name[8];
        app_register_address_format(addresses[i], name, sizeof(name));
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        cJSON_AddStringToObject(entry, "address", name);
        if (app_register_area_is_bit(addresses[i].area)) {
            cJSON_AddBoolToObject(entry, "value", values[i] != 0);
        } else {
            cJSON_AddNumberToObject(entry, "value", values[i]);
        }
        cJSON_AddItemToArray(results, entry);
    }
    free(addresses);
    free(values);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return send_error(request, err == ESP_ERR_NO_MEM ? HTTPD_500 : HTTPD_400,
                          err == ESP_ERR_NO_MEM ? "REG_NO_MEMORY" : "REG_INVALID_ADDRESS",
                          err == ESP_ERR_NO_MEM ? "out of memory" : "invalid register address");
    }
    cJSON_AddTrueToObject(root, "ok");
    add_revision(root, revision);
    return send_json(request, HTTPD_200, root);
}

static esp_err_t write_handler(httpd_req_t *request)
{
    reg_http_t *http = request->user_ctx;
    cJSON *body = read_body(request);
    if (!body) return send_error(request, HTTPD_400, "REG_INVALID_REQUEST",
                                 "invalid or oversized JSON body");
    cJSON *list = cJSON_GetObjectItemCaseSensitive(body, "writes");
    cJSON *start = cJSON_GetObjectItemCaseSensitive(body, "start");
    cJSON *range_values = cJSON_GetObjectItemCaseSensitive(body, "values");
    app_register_address_t first = {0};
    bool range = !list && parse_address(start, &first) &&
                 cJSON_IsArray(range_values);
    size_t count = range ? cJSON_GetArraySize(range_values) :
                   cJSON_IsArray(list) && !start && !range_values
                       ? cJSON_GetArraySize(list) : 0;
    if (count == 0 || count > CONFIG_APP_REGISTERS_MAX_TRANSACTION ||
        (range && count > app_registers_area_count(first.area) - first.index)) {
        cJSON_Delete(body);
        return send_error(request, HTTPD_400, "REG_INVALID_REQUEST",
                          "invalid register write request");
    }
    app_register_write_t *writes = calloc(count, sizeof(*writes));
    if (!writes) {
        cJSON_Delete(body);
        return send_error(request, HTTPD_500, "REG_NO_MEMORY", "out of memory");
    }
    bool valid = true;
    for (size_t i = 0; i < count; ++i) {
        cJSON *value;
        if (range) {
            writes[i].address = (app_register_address_t){first.area,
                                                          (uint16_t)(first.index + i)};
            value = cJSON_GetArrayItem(range_values, i);
        } else {
            cJSON *entry = cJSON_GetArrayItem(list, i);
            if (!cJSON_IsObject(entry) ||
                !parse_address(cJSON_GetObjectItemCaseSensitive(entry, "address"),
                               &writes[i].address)) {
                valid = false;
                break;
            }
            value = cJSON_GetObjectItemCaseSensitive(entry, "value");
        }
        if (!parse_value(value, writes[i].address, &writes[i].value)) {
            valid = false;
            break;
        }
    }
    uint64_t revision = 0;
    esp_err_t err = valid ? app_registers_write_many(http->registers, writes,
                                count, APP_REGISTER_SOURCE_HTTP, &revision)
                          : ESP_ERR_INVALID_ARG;
    free(writes);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        const char *code = err == APP_REGISTERS_ERR_READ_ONLY ? "REG_READ_ONLY" :
                           err == APP_REGISTERS_ERR_DUPLICATE ? "REG_DUPLICATE" :
                           err == ESP_ERR_NO_MEM ? "REG_NO_MEMORY" :
                           "REG_INVALID_REQUEST";
        return send_error(request,
                          err == APP_REGISTERS_ERR_READ_ONLY ? "403 Forbidden" :
                          err == APP_REGISTERS_ERR_DUPLICATE ? "409 Conflict" :
                          err == ESP_ERR_NO_MEM ? HTTPD_500 : HTTPD_400,
                          code, "register write failed");
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) return send_error(request, HTTPD_500, "REG_NO_MEMORY", "out of memory");
    cJSON_AddTrueToObject(root, "ok");
    add_revision(root, revision);
    return send_json(request, HTTPD_200, root);
}

static void on_register_change(const app_register_event_t *event, void *opaque)
{
    reg_http_t *http = opaque;
    for (size_t i = 0; i < event->change_count; ++i) {
        portENTER_CRITICAL(&http->lock);
        reg_change_t *slot = &http->ring[http->next_sequence % REG_EVENT_RING_SIZE];
        *slot = (reg_change_t){event->revision, event->source,
                               event->changes[i]};
        ++http->next_sequence;
        portEXIT_CRITICAL(&http->lock);
    }
}

static void events_task(void *argument)
{
    reg_event_client_t *client = argument;
    httpd_req_t *request = client->request;
    reg_http_t *http = request->user_ctx;
    uint64_t cursor;
    portENTER_CRITICAL(&http->lock);
    cursor = http->next_sequence;
    portEXIT_CRITICAL(&http->lock);
    httpd_resp_set_type(request, "text/event-stream");
    httpd_resp_set_hdr(request, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(request, "Connection", "keep-alive");
    esp_err_t err = httpd_resp_send_chunk(request, ": connected\n\n", 13);
    TickType_t last_send = xTaskGetTickCount();
    int socket = httpd_req_to_sockfd(request);
    while (err == ESP_OK) {
        // Filtered streams can be silent for a long time. Release closed
        // clients promptly instead of waiting for the next heartbeat write.
        if (!reg_http_connection_alive(socket)) break;
        uint64_t dropped = 0;
        reg_change_t change = {0};
        bool ready = false;
        portENTER_CRITICAL(&http->lock);
        uint64_t current = http->next_sequence;
        if (current - cursor > REG_EVENT_RING_SIZE) {
            dropped = current - cursor - REG_EVENT_RING_SIZE;
            cursor = current - REG_EVENT_RING_SIZE;
        } else if (cursor < current) {
            change = http->ring[cursor % REG_EVENT_RING_SIZE];
            ++cursor;
            ready = true;
        }
        portEXIT_CRITICAL(&http->lock);
        char line[256];
        int length = 0;
        if (dropped) {
            length = snprintf(line, sizeof(line),
                              "event: dropped\ndata: %" PRIu64 "\n\n", dropped);
        } else if (ready) {
            if (!reg_http_filter_matches(&client->filter,
                                          change.change.address)) continue;
            char address[8];
            app_register_address_format(change.change.address,
                                        address, sizeof(address));
            bool bit = app_register_area_is_bit(change.change.address.area);
            char new_value[16];
            if (bit) {
                snprintf(new_value, sizeof(new_value), "%s",
                         change.change.value ? "true" : "false");
            } else {
                snprintf(new_value, sizeof(new_value), "%u",
                         change.change.value);
            }
            length = snprintf(line, sizeof(line),
                "event: change\ndata: {\"r\":\"%" PRIu64
                "\",\"a\":\"%s\",\"v\":%s}\n\n",
                change.revision, address, new_value);
        } else if (xTaskGetTickCount() - last_send >= pdMS_TO_TICKS(15000)) {
            length = snprintf(line, sizeof(line), ": ping\n\n");
        }
        if (length > 0 && (size_t)length < sizeof(line)) {
            err = httpd_resp_send_chunk(request, line, length);
            last_send = xTaskGetTickCount();
        } else if (!ready && !dropped) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    portENTER_CRITICAL(&http->lock);
    if (http->active_clients > 0) --http->active_clients;
    portEXIT_CRITICAL(&http->lock);
    httpd_req_async_handler_complete(request);
    app_http_server_worker_release(http->http_server);
    reg_http_filter_destroy(&client->filter);
    free(client);
    vTaskDelete(NULL);
}

static esp_err_t events_handler(httpd_req_t *request)
{
    reg_http_t *http = request->user_ctx;
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length > CONFIG_HTTPD_MAX_URI_LEN) {
        return send_error(request, HTTPD_400, "REG_INVALID_REQUEST",
                          "register event subscription is too long");
    }
    char *query = NULL;
    if (query_length) {
        query = malloc(query_length + 1);
        if (!query) return send_error(request, HTTPD_500, "REG_NO_MEMORY", "out of memory");
        if (httpd_req_get_url_query_str(request, query, query_length + 1) != ESP_OK) {
            free(query);
            return send_error(request, HTTPD_400, "REG_INVALID_REQUEST",
                              "invalid register event subscription");
        }
    }
    reg_http_filter_t filter = {0};
    esp_err_t parse_error = reg_http_filter_parse(query, &filter);
    free(query);
    if (parse_error != ESP_OK) {
        reg_http_filter_destroy(&filter);
        return send_error(request,
                          parse_error == ESP_ERR_NO_MEM ? HTTPD_500 : HTTPD_400,
                          parse_error == ESP_ERR_NO_MEM ? "REG_NO_MEMORY" : "REG_INVALID_REQUEST",
                          parse_error == ESP_ERR_NO_MEM ? "out of memory" :
                          "invalid register event subscription");
    }
    if (!app_http_server_worker_try_acquire(http->http_server)) {
        reg_http_filter_destroy(&filter);
        return send_error(request, "429 Too Many Requests", "REG_BUSY",
                          "too many long-lived HTTP requests");
    }
    bool acquired = false;
    portENTER_CRITICAL(&http->lock);
    if (http->active_clients < CONFIG_REG_HTTP_MAX_SSE_CLIENTS) {
        ++http->active_clients;
        acquired = true;
    }
    portEXIT_CRITICAL(&http->lock);
    if (!acquired) {
        app_http_server_worker_release(http->http_server);
        reg_http_filter_destroy(&filter);
        return send_error(request, "429 Too Many Requests", "REG_BUSY",
                          "too many register watchers");
    }
    reg_event_client_t *client = calloc(1, sizeof(*client));
    if (!client) {
        portENTER_CRITICAL(&http->lock);
        --http->active_clients;
        portEXIT_CRITICAL(&http->lock);
        app_http_server_worker_release(http->http_server);
        reg_http_filter_destroy(&filter);
        return send_error(request, HTTPD_500, "REG_NO_MEMORY", "out of memory");
    }
    client->filter = filter;
    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    client->request = async_request;
    if (err == ESP_OK && xTaskCreate(events_task, "reg_events", 4096,
                                     client, 4, NULL) == pdPASS) {
        return ESP_OK;
    }
    if (async_request) httpd_req_async_handler_complete(async_request);
    reg_http_filter_destroy(&client->filter);
    free(client);
    portENTER_CRITICAL(&http->lock);
    --http->active_clients;
    portEXIT_CRITICAL(&http->lock);
    app_http_server_worker_release(http->http_server);
    return send_error(request, HTTPD_500, "REG_NO_MEMORY",
                      "cannot start register watcher");
}

esp_err_t reg_http_start(const reg_http_config_t *config, reg_http_t **out_http)
{
    if (!config || !config->http_server || !config->registers || !out_http) {
        return ESP_ERR_INVALID_ARG;
    }
    reg_http_t *http = calloc(1, sizeof(*http));
    if (!http) return ESP_ERR_NO_MEM;
    http->http_server = config->http_server;
    http->registers = config->registers;
    http->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    const httpd_uri_t routes[] = {
        {.uri = REG_ROOT, .method = HTTP_GET, .handler = info_handler,
         .user_ctx = http},
        {.uri = REG_ROOT "/read", .method = HTTP_POST, .handler = read_handler,
         .user_ctx = http},
        {.uri = REG_ROOT "/write", .method = HTTP_POST, .handler = write_handler,
         .user_ctx = http},
        {.uri = REG_ROOT "/events", .method = HTTP_GET, .handler = events_handler,
         .user_ctx = http},
    };
    esp_err_t err = app_registers_subscribe(http->registers,
                                            on_register_change, http,
                                            &http->subscriber_id);
    if (err == ESP_OK) {
        err = app_http_server_register(http->http_server, REG_HTTP_OWNER, routes,
                                       sizeof(routes) / sizeof(routes[0]));
        if (err != ESP_OK) {
            app_registers_unsubscribe(http->registers, http->subscriber_id);
        }
    }
    if (err != ESP_OK) {
        free(http);
        return err;
    }
    *out_http = http;
    return ESP_OK;
}

#endif
