#include "js_http_internal.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if CONFIG_JS_HTTP_EVAL_ENABLED

static esp_err_t send_json(httpd_req_t *request, const char *status,
                           cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) {
        httpd_resp_set_status(request, HTTPD_500);
        return httpd_resp_sendstr(request,
                                  "{\"ok\":false,\"error\":{\"name\":"
                                  "\"OutOfMemoryError\",\"message\":"
                                  "\"cannot encode response\"}}");
    }
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(request, body, strlen(body));
    cJSON_free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *request, const char *status,
                            const char *name, const char *message)
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
    cJSON_AddStringToObject(error, "name", name);
    cJSON_AddStringToObject(error, "message", message);
    return send_json(request, status, root);
}

static esp_err_t receive_body(httpd_req_t *request, char **out_body)
{
    if (request->content_len == 0 ||
        request->content_len > CONFIG_JS_HTTP_EVAL_MAX_BODY_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    char *body = malloc(request->content_len + 1);
    if (!body) {
        return ESP_ERR_NO_MEM;
    }
    size_t received = 0;
    while (received < request->content_len) {
        int count = httpd_req_recv(request, body + received,
                                   request->content_len - received);
        if (count <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    *out_body = body;
    return ESP_OK;
}

static bool parse_eval_options(cJSON *json, js_eval_options_t *options,
                               const char **error)
{
    if (!cJSON_IsObject(json)) {
        *error = "request body must be an object";
        return false;
    }
    cJSON *source = cJSON_GetObjectItemCaseSensitive(json, "source");
    if (!cJSON_IsString(source) || !source->valuestring ||
        source->valuestring[0] == '\0') {
        *error = "source must be a non-empty string";
        return false;
    }
    size_t source_size = strlen(source->valuestring);
    if (source_size > CONFIG_JS_HTTP_EVAL_MAX_SOURCE_SIZE) {
        *error = "source exceeds the configured limit";
        return false;
    }

    js_eval_mode_t mode = JS_EVAL_MODE_EXPR;
    cJSON *mode_json = cJSON_GetObjectItemCaseSensitive(json, "mode");
    if (mode_json) {
        if (!cJSON_IsString(mode_json) || !mode_json->valuestring) {
            *error = "mode must be expr or script";
            return false;
        }
        if (strcmp(mode_json->valuestring, "expr") == 0) {
            mode = JS_EVAL_MODE_EXPR;
        } else if (strcmp(mode_json->valuestring, "script") == 0) {
            mode = JS_EVAL_MODE_SCRIPT;
        } else {
            *error = "mode must be expr or script";
            return false;
        }
    }

    bool await_result = true;
    cJSON *await_json = cJSON_GetObjectItemCaseSensitive(json, "await");
    if (await_json) {
        if (!cJSON_IsBool(await_json)) {
            *error = "await must be a boolean";
            return false;
        }
        await_result = cJSON_IsTrue(await_json);
    }

    uint32_t timeout_ms = CONFIG_JS_HTTP_EVAL_DEFAULT_TIMEOUT_MS;
    cJSON *timeout = cJSON_GetObjectItemCaseSensitive(json, "timeoutMs");
    if (timeout) {
        if (!cJSON_IsNumber(timeout) || timeout->valuedouble < 1 ||
            timeout->valuedouble > CONFIG_JS_HTTP_EVAL_MAX_TIMEOUT_MS) {
            *error = "timeoutMs is outside the configured range";
            return false;
        }
        timeout_ms = (uint32_t)timeout->valuedouble;
        if ((double)timeout_ms != timeout->valuedouble) {
            *error = "timeoutMs must be an integer";
            return false;
        }
    }

    *options = (js_eval_options_t) {
        .source = source->valuestring,
        .source_size = source_size,
        .mode = mode,
        .await_result = await_result,
        .timeout_ms = timeout_ms,
        .max_result_size = CONFIG_JS_HTTP_EVAL_MAX_RESULT_SIZE,
    };
    return true;
}

static cJSON *result_to_json(const js_eval_result_t *result)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "ok", result->status == JS_EVAL_RESULT_OK);
    cJSON_AddBoolToObject(root, "async", result->async);
    if (result->truncated) {
        cJSON_AddTrueToObject(root, "truncated");
    }

    if (result->status == JS_EVAL_RESULT_OK) {
        cJSON_AddStringToObject(root, "type",
                                result->type ? result->type : "unknown");
        cJSON *value = result->value_json
                           ? cJSON_Parse(result->value_json)
                           : cJSON_CreateNull();
        if (!value) {
            value = cJSON_CreateNull();
        }
        cJSON_AddItemToObject(root, "value", value);
        if (result->display) {
            cJSON_AddStringToObject(root, "display", result->display);
        }
        return root;
    }

    cJSON *error = cJSON_CreateObject();
    if (!error) {
        cJSON_Delete(root);
        return NULL;
    }
    cJSON_AddItemToObject(root, "error", error);
    cJSON_AddStringToObject(error, "name",
                            result->error_name ? result->error_name : "Error");
    cJSON_AddStringToObject(
        error, "message", result->error_message
                              ? result->error_message
                              : "JavaScript evaluation failed");
    if (result->error_stack) {
        cJSON_AddStringToObject(error, "stack", result->error_stack);
    }
    return root;
}

static void eval_task(void *arg)
{
    httpd_req_t *request = arg;
    js_http_t *http = request->user_ctx;
    char *body = NULL;
    cJSON *json = NULL;
    js_eval_request_t *eval_request = NULL;
    js_eval_result_t result = {0};

    esp_err_t err = receive_body(request, &body);
    if (err == ESP_ERR_INVALID_SIZE) {
        send_error(request, "413 Content Too Large", "RequestTooLargeError",
                   "request body exceeds the configured limit");
        goto complete;
    }
    if (err != ESP_OK) {
        send_error(request, HTTPD_400, "RequestError",
                   "cannot read request body");
        goto complete;
    }

    json = cJSON_ParseWithLength(body, request->content_len);
    if (!json) {
        send_error(request, HTTPD_400, "SyntaxError", "invalid JSON");
        goto complete;
    }
    js_eval_options_t options;
    const char *validation_error = NULL;
    if (!parse_eval_options(json, &options, &validation_error)) {
        const char *status = strstr(validation_error, "exceeds")
                                 ? "413 Content Too Large"
                                 : HTTPD_400;
        send_error(request, status, "ValidationError", validation_error);
        goto complete;
    }

    err = js_runtime_eval_submit(http->runtime, &options, &eval_request);
    cJSON_Delete(json);
    json = NULL;
    free(body);
    body = NULL;
    if (err != ESP_OK) {
        send_error(request, "503 Service Unavailable", "RuntimeUnavailableError",
                   "cannot queue JavaScript evaluation");
        goto complete;
    }

    TickType_t wait = pdMS_TO_TICKS(options.timeout_ms + 1000);
    err = js_runtime_eval_receive(eval_request, &result, wait);
    js_runtime_eval_request_destroy(eval_request);
    eval_request = NULL;
    if (err != ESP_OK) {
        send_error(request, "504 Gateway Timeout", "TimeoutError",
                   "evaluation completion timed out");
        goto complete;
    }

    const char *status = HTTPD_200;
    if (result.status == JS_EVAL_RESULT_TIMEOUT) {
        status = "504 Gateway Timeout";
    } else if (result.status == JS_EVAL_RESULT_RUNTIME_STOPPED) {
        status = "503 Service Unavailable";
    }
    cJSON *response = result_to_json(&result);
    js_runtime_eval_result_free(&result);
    if (!response) {
        send_error(request, HTTPD_500, "OutOfMemoryError",
                   "cannot encode evaluation result");
    } else {
        send_json(request, status, response);
    }

complete:
    if (eval_request) {
        js_runtime_eval_request_destroy(eval_request);
    }
    js_runtime_eval_result_free(&result);
    cJSON_Delete(json);
    free(body);
    httpd_req_async_handler_complete(request);
    js_http_eval_release(http);
    vTaskDelete(NULL);
}

static esp_err_t eval_handler(httpd_req_t *request)
{
    char content_type[48];
    if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type,
                                    sizeof(content_type)) != ESP_OK ||
        strncmp(content_type, "application/json", 16) != 0 ||
        (content_type[16] != '\0' && content_type[16] != ';')) {
        return send_error(request, "415 Unsupported Media Type",
                          "UnsupportedMediaTypeError",
                          "Content-Type must be application/json");
    }
    if (request->content_len == 0) {
        return send_error(request, HTTPD_400, "RequestError",
                          "request body must not be empty");
    }
    if (request->content_len > CONFIG_JS_HTTP_EVAL_MAX_BODY_SIZE) {
        return send_error(request, "413 Content Too Large",
                          "RequestTooLargeError",
                          "request body exceeds the configured limit");
    }
    if (!js_http_eval_try_acquire(request->user_ctx)) {
        return send_error(request, "429 Too Many Requests", "BusyError",
                          "too many JavaScript evaluations");
    }

    httpd_req_t *async_request = NULL;
    esp_err_t err = httpd_req_async_handler_begin(request, &async_request);
    if (err != ESP_OK) {
        js_http_eval_release(request->user_ctx);
        return err;
    }
    if (xTaskCreate(eval_task, "js_http_eval",
                    CONFIG_JS_HTTP_EVAL_TASK_STACK_SIZE, async_request,
                    CONFIG_JS_HTTP_EVAL_TASK_PRIORITY, NULL) != pdPASS) {
        httpd_req_async_handler_complete(async_request);
        js_http_eval_release(request->user_ctx);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif

esp_err_t js_http_eval_register(js_http_t *http)
{
#if CONFIG_JS_HTTP_EVAL_ENABLED
    const httpd_uri_t route = {
        .uri = "/api/v1/quickjs/eval",
        .method = HTTP_POST,
        .handler = eval_handler,
        .user_ctx = http,
    };
    return app_http_server_register(http->http_server, JS_HTTP_OWNER,
                                    &route, 1);
#else
    (void)http;
    return ESP_OK;
#endif
}
