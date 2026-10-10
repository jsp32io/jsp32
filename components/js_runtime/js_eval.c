#include "js_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/semphr.h"
#include "js_runtime_internal.h"

#define JS_EVENT_INTERNAL_EVAL 2
#define EVAL_FILENAME "<http-eval>"

typedef struct {
    js_eval_request_t *request;
    bool handled;
} js_eval_event_payload_t;

struct js_eval_request {
    js_runtime_t *runtime;
    portMUX_TYPE lock;
    SemaphoreHandle_t completed;
    uint32_t references;
    char *source;
    size_t source_size;
    js_eval_mode_t mode;
    bool await_result;
    size_t max_result_size;
    int64_t deadline_us;
    bool cancelled;
    bool finished;
    js_eval_result_t result;

    /* EventLoop-owned fields. */
    JSValue promise;
    js_eval_request_t *next;
};

static void result_clear(js_eval_result_t *result)
{
    free(result->type);
    free(result->value_json);
    free(result->display);
    free(result->error_name);
    free(result->error_message);
    free(result->error_stack);
    memset(result, 0, sizeof(*result));
}

void js_runtime_eval_result_free(js_eval_result_t *result)
{
    if (result) {
        result_clear(result);
    }
}

static void request_add_ref(js_eval_request_t *request)
{
    portENTER_CRITICAL(&request->lock);
    ++request->references;
    portEXIT_CRITICAL(&request->lock);
}

static void request_release(js_eval_request_t *request)
{
    bool release = false;
    portENTER_CRITICAL(&request->lock);
    if (--request->references == 0) {
        release = true;
    }
    portEXIT_CRITICAL(&request->lock);
    if (!release) {
        return;
    }
    result_clear(&request->result);
    free(request->source);
    vSemaphoreDelete(request->completed);
    free(request);
}

static bool request_is_cancelled(js_eval_request_t *request)
{
    portENTER_CRITICAL(&request->lock);
    bool cancelled = request->cancelled;
    portEXIT_CRITICAL(&request->lock);
    return cancelled;
}

static void request_complete(js_eval_request_t *request,
                             js_eval_result_t *result)
{
    bool notify = false;
    portENTER_CRITICAL(&request->lock);
    if (!request->cancelled && !request->finished) {
        request->result = *result;
        memset(result, 0, sizeof(*result));
        request->finished = true;
        notify = true;
    }
    portEXIT_CRITICAL(&request->lock);
    if (notify) {
        xSemaphoreGive(request->completed);
    } else {
        result_clear(result);
    }
}

static char *duplicate_limited(const char *value, size_t maximum,
                               bool *truncated)
{
    if (!value) {
        return NULL;
    }
    size_t length = strlen(value);
    if (length > maximum) {
        length = maximum;
        *truncated = true;
    }
    char *copy = malloc(length + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, length);
    copy[length] = '\0';
    return copy;
}

static void clear_pending_exception(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
}

static char *value_to_display(JSContext *ctx, JSValueConst value,
                              size_t maximum, bool *truncated)
{
    const char *text = JS_ToCString(ctx, value);
    if (!text) {
        clear_pending_exception(ctx);
        return NULL;
    }
    char *copy = duplicate_limited(text, maximum, truncated);
    JS_FreeCString(ctx, text);
    return copy;
}

static char *property_to_string(JSContext *ctx, JSValueConst object,
                                const char *name, size_t maximum,
                                bool *truncated)
{
    JSValue property = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsException(property)) {
        clear_pending_exception(ctx);
        return NULL;
    }
    char *result = NULL;
    if (!JS_IsUndefined(property) && !JS_IsNull(property)) {
        result = value_to_display(ctx, property, maximum, truncated);
    }
    JS_FreeValue(ctx, property);
    return result;
}

static const char *value_type(JSContext *ctx, JSValueConst value,
                              bool is_promise)
{
    if (is_promise) {
        return "promise";
    }
    if (JS_IsUndefined(value)) {
        return "undefined";
    }
    if (JS_IsNull(value)) {
        return "null";
    }
    if (JS_IsBool(value)) {
        return "boolean";
    }
    if (JS_IsNumber(value)) {
        return "number";
    }
    if (JS_IsBigInt(ctx, value)) {
        return "bigint";
    }
    if (JS_IsString(value)) {
        return "string";
    }
    if (JS_IsSymbol(value)) {
        return "symbol";
    }
    if (JS_IsFunction(ctx, value)) {
        return "function";
    }
    return "object";
}

static js_eval_result_t format_success(js_eval_request_t *request,
                                       JSValueConst value, bool async,
                                       bool is_promise)
{
    JSContext *ctx = request->runtime->ctx;
    js_eval_result_t result = {
        .status = JS_EVAL_RESULT_OK,
        .async = async,
    };
    js_runtime_budget_begin(request->runtime);
    result.type = strdup(value_type(ctx, value, is_promise));
    result.display = value_to_display(ctx, value, request->max_result_size,
                                      &result.truncated);

    JSValue json = JS_JSONStringify(ctx, value, JS_UNDEFINED, JS_UNDEFINED);
    if (JS_IsException(json)) {
        clear_pending_exception(ctx);
    } else if (!JS_IsUndefined(json)) {
        const char *encoded = JS_ToCString(ctx, json);
        if (encoded) {
            size_t length = strlen(encoded);
            if (length <= request->max_result_size) {
                result.value_json = strdup(encoded);
            } else {
                result.truncated = true;
            }
            JS_FreeCString(ctx, encoded);
        } else {
            clear_pending_exception(ctx);
        }
    }
    JS_FreeValue(ctx, json);
    js_runtime_budget_end(request->runtime);
    return result;
}

static js_eval_result_t format_error(js_eval_request_t *request,
                                     JSValueConst error,
                                     const char *fallback_name,
                                     bool async)
{
    JSContext *ctx = request->runtime->ctx;
    js_eval_result_t result = {
        .status = JS_EVAL_RESULT_EXCEPTION,
        .async = async,
    };
    js_runtime_budget_begin(request->runtime);
    if (JS_IsError(ctx, error)) {
        result.error_name = property_to_string(
            ctx, error, "name", request->max_result_size, &result.truncated);
        result.error_message = property_to_string(
            ctx, error, "message", request->max_result_size,
            &result.truncated);
        result.error_stack = property_to_string(
            ctx, error, "stack", request->max_result_size, &result.truncated);
    }
    if (!result.error_name) {
        result.error_name = strdup(fallback_name);
    }
    if (!result.error_message) {
        result.error_message = value_to_display(
            ctx, error, request->max_result_size, &result.truncated);
    }
    if (!result.error_message) {
        result.error_message = strdup("JavaScript evaluation failed");
    }
    js_runtime_budget_end(request->runtime);
    return result;
}

static js_eval_result_t timeout_result(void)
{
    js_eval_result_t result = {
        .status = JS_EVAL_RESULT_TIMEOUT,
        .async = true,
        .error_name = strdup("TimeoutError"),
        .error_message = strdup("evaluation timed out"),
    };
    return result;
}

static js_eval_result_t stopped_result(void)
{
    js_eval_result_t result = {
        .status = JS_EVAL_RESULT_RUNTIME_STOPPED,
        .error_name = strdup("RuntimeStoppedError"),
        .error_message = strdup("JavaScript runtime stopped"),
    };
    return result;
}

static void eval_event_payload_free(void *opaque)
{
    js_eval_event_payload_t *payload = opaque;
    if (!payload->handled) {
        js_eval_result_t result = stopped_result();
        request_complete(payload->request, &result);
    }
    request_release(payload->request);
    free(payload);
}

static JSValue evaluate_request(js_eval_request_t *request)
{
    js_runtime_t *runtime = request->runtime;
    const char *source = request->source;
    size_t source_size = request->source_size;
    char *wrapped = NULL;
    int flags = JS_EVAL_TYPE_GLOBAL;

    if (request->mode == JS_EVAL_MODE_EXPR) {
        static const char prefix[] = "(async()=>(";
        static const char suffix[] = "\n))()";
        size_t wrapped_size = sizeof(prefix) - 1 + source_size +
                              sizeof(suffix) - 1;
        wrapped = malloc(wrapped_size + 1);
        if (!wrapped) {
            return JS_ThrowOutOfMemory(runtime->ctx);
        }
        memcpy(wrapped, prefix, sizeof(prefix) - 1);
        memcpy(wrapped + sizeof(prefix) - 1, source, source_size);
        memcpy(wrapped + sizeof(prefix) - 1 + source_size,
               suffix, sizeof(suffix));
        source = wrapped;
        source_size = wrapped_size;
    } else {
        flags |= JS_EVAL_FLAG_ASYNC;
    }

    js_runtime_budget_begin(runtime);
    JSValue value = JS_Eval(runtime->ctx, source, source_size,
                            EVAL_FILENAME, flags);
    js_runtime_budget_end(runtime);
    free(wrapped);
    return value;
}

static void settle_request(js_eval_request_t *request, JSValue value,
                           bool rejected, bool async)
{
    js_eval_result_t result = rejected
        ? format_error(request, value, "PromiseRejection", async)
        : format_success(request, value, async, false);
    JS_FreeValue(request->runtime->ctx, value);
    request_complete(request, &result);
}

static void eval_event_handler(js_runtime_t *runtime,
                               const js_event_t *event, void *opaque)
{
    (void)opaque;
    js_eval_event_payload_t *payload = event->payload;
    payload->handled = true;
    js_eval_request_t *request = payload->request;
    if (request_is_cancelled(request)) {
        return;
    }
    if (esp_timer_get_time() >= request->deadline_us) {
        js_eval_result_t result = timeout_result();
        request_complete(request, &result);
        return;
    }

    JSValue value = evaluate_request(request);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(runtime->ctx);
        js_eval_result_t result = format_error(
            request, error, "Error", false);
        JS_FreeValue(runtime->ctx, error);
        request_complete(request, &result);
        return;
    }

    int promise_state = (int)JS_PromiseState(runtime->ctx, value);
    bool is_promise = promise_state >= 0;
    if (!request->await_result || !is_promise) {
        js_eval_result_t result = format_success(
            request, value, is_promise, is_promise);
        JS_FreeValue(runtime->ctx, value);
        request_complete(request, &result);
        return;
    }
    if (promise_state != JS_PROMISE_PENDING) {
        JSValue settled = JS_PromiseResult(runtime->ctx, value);
        JS_FreeValue(runtime->ctx, value);
        settle_request(request, settled,
                       promise_state == JS_PROMISE_REJECTED, true);
        return;
    }

    request->promise = value;
    request_add_ref(request);
    request->next = runtime->pending_evals;
    runtime->pending_evals = request;
}

esp_err_t js_eval_init(js_runtime_t *runtime)
{
    runtime->pending_evals = NULL;
    return js_event_register_handler(runtime, JS_EVENT_INTERNAL_EVAL,
                                     eval_event_handler, NULL);
}

void js_eval_process_pending(js_runtime_t *runtime)
{
    int64_t now = esp_timer_get_time();
    js_eval_request_t **link = &runtime->pending_evals;
    while (*link) {
        js_eval_request_t *request = *link;
        bool remove = false;
        if (request_is_cancelled(request)) {
            remove = true;
        } else if (now >= request->deadline_us) {
            js_eval_result_t result = timeout_result();
            request_complete(request, &result);
            remove = true;
        } else {
            int state = (int)JS_PromiseState(runtime->ctx, request->promise);
            if (state != JS_PROMISE_PENDING) {
                JSValue settled = JS_PromiseResult(runtime->ctx,
                                                    request->promise);
                settle_request(request, settled,
                               state == JS_PROMISE_REJECTED, true);
                remove = true;
            }
        }
        if (!remove) {
            link = &request->next;
            continue;
        }
        *link = request->next;
        JS_FreeValue(runtime->ctx, request->promise);
        request->promise = JS_UNDEFINED;
        request_release(request);
    }
}

static TickType_t deadline_to_ticks(int64_t deadline_us)
{
    int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint64_t wait_ms = ((uint64_t)remaining_us + 999) / 1000;
    uint64_t ticks = (wait_ms * configTICK_RATE_HZ + 999) / 1000;
    if (ticks == 0) {
        ticks = 1;
    }
    return ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)ticks;
}

TickType_t js_eval_next_wait(js_runtime_t *runtime)
{
    TickType_t wait = portMAX_DELAY;
    for (js_eval_request_t *request = runtime->pending_evals;
         request; request = request->next) {
        TickType_t candidate = deadline_to_ticks(request->deadline_us);
        if (candidate < wait) {
            wait = candidate;
        }
    }
    return wait;
}

void js_eval_deinit(js_runtime_t *runtime)
{
    js_eval_request_t *request = runtime->pending_evals;
    runtime->pending_evals = NULL;
    while (request) {
        js_eval_request_t *next = request->next;
        JS_FreeValue(runtime->ctx, request->promise);
        js_eval_result_t result = stopped_result();
        request_complete(request, &result);
        request_release(request);
        request = next;
    }
}

esp_err_t js_runtime_eval_submit(js_runtime_t *runtime,
                                 const js_eval_options_t *options,
                                 js_eval_request_t **out_request)
{
    if (!runtime || !options || !out_request || !options->source ||
        options->source_size == 0 || options->source_size == SIZE_MAX ||
        options->timeout_ms == 0 ||
        options->max_result_size == 0 ||
        (options->mode != JS_EVAL_MODE_EXPR &&
         options->mode != JS_EVAL_MODE_SCRIPT)) {
        return ESP_ERR_INVALID_ARG;
    }

    js_eval_request_t *request = calloc(1, sizeof(*request));
    js_eval_event_payload_t *payload = calloc(1, sizeof(*payload));
    if (!request || !payload) {
        free(request);
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    request->source = malloc(options->source_size + 1);
    request->completed = xSemaphoreCreateBinary();
    if (!request->source || !request->completed) {
        free(request->source);
        if (request->completed) {
            vSemaphoreDelete(request->completed);
        }
        free(request);
        free(payload);
        return ESP_ERR_NO_MEM;
    }
    memcpy(request->source, options->source, options->source_size);
    request->source[options->source_size] = '\0';
    request->runtime = runtime;
    request->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    request->references = 2; /* Caller and queued event. */
    request->source_size = options->source_size;
    request->mode = options->mode;
    request->await_result = options->await_result;
    request->max_result_size = options->max_result_size;
    request->deadline_us = esp_timer_get_time() +
                           (int64_t)options->timeout_ms * 1000;
    request->promise = JS_UNDEFINED;
    payload->request = request;

    js_event_t event = {
        .type = JS_EVENT_INTERNAL_EVAL,
        .payload = payload,
        .payload_size = sizeof(*payload),
        .free_payload = eval_event_payload_free,
    };
    esp_err_t err = js_event_post(runtime, &event, 0);
    if (err != ESP_OK) {
        request_release(request);
        return err;
    }
    *out_request = request;
    return ESP_OK;
}

esp_err_t js_runtime_eval_receive(js_eval_request_t *request,
                                  js_eval_result_t *out_result,
                                  TickType_t wait_ticks)
{
    if (!request || !out_result) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(request->completed, wait_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&request->lock);
    if (!request->finished) {
        portEXIT_CRITICAL(&request->lock);
        return ESP_ERR_INVALID_STATE;
    }
    *out_result = request->result;
    memset(&request->result, 0, sizeof(request->result));
    portEXIT_CRITICAL(&request->lock);
    return ESP_OK;
}

void js_runtime_eval_request_destroy(js_eval_request_t *request)
{
    if (!request) {
        return;
    }
    portENTER_CRITICAL(&request->lock);
    request->cancelled = true;
    portEXIT_CRITICAL(&request->lock);
    request_release(request);
}
