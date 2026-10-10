#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_filesystem.h"
#include "freertos/FreeRTOS.h"
#include "js_event.h"
#include "js_request.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_runtime js_runtime_t;
typedef struct js_console_subscription js_console_subscription_t;
typedef struct js_eval_request js_eval_request_t;

#define JS_RUNTIME_CONSOLE_LINE_MAX 1024
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *runtime,
                                               JSContext *ctx,
                                               void *opaque);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *runtime,
                                            JSContext *ctx,
                                            void *opaque);

/* Register modules before js_runtime_start(); callbacks run in EventLoop task. */
esp_err_t js_runtime_register_module(js_runtime_module_init_fn init,
                                     js_runtime_module_deinit_fn deinit,
                                     void *opaque);

esp_err_t js_runtime_start(const char *script, size_t script_size,
                           const char *filename, app_filesystem_t *filesystem,
                           js_runtime_t **out_runtime);
esp_err_t js_runtime_stop(js_runtime_t *runtime, TickType_t wait_ticks);

bool js_runtime_is_owner(const js_runtime_t *runtime);
JSContext *js_runtime_context(js_runtime_t *runtime);
/* EventLoop only. Adds automatic deadlines and cleanup. Table storage must
 * remain valid until runtime shutdown; do not reinitialize a tracked table. */
esp_err_t js_runtime_track_requests(js_runtime_t *runtime, js_request_table_t *table);
uint32_t js_runtime_dropped_events(js_runtime_t *runtime);

/* Call a JavaScript function with the runtime execution budget. EventLoop only. */
bool js_runtime_call(js_runtime_t *runtime, JSValueConst callback,
                     JSValueConst this_value, int argc,
                     JSValueConst *argv);

/* Console subscriptions are safe to consume from a task other than EventLoop. */
esp_err_t js_runtime_console_subscribe(js_runtime_t *runtime,
                                       js_console_subscription_t **out);
/* ESP_OK returns a line; ESP_ERR_NOT_FOUND reports only out_dropped. */
esp_err_t js_runtime_console_receive(js_console_subscription_t *subscription,
                                     char *buffer, size_t buffer_size,
                                     uint32_t *out_dropped,
                                     TickType_t wait_ticks);
void js_runtime_console_unsubscribe(js_console_subscription_t *subscription);

typedef enum {
    JS_EVAL_MODE_EXPR,
    JS_EVAL_MODE_SCRIPT,
} js_eval_mode_t;

typedef enum {
    JS_EVAL_RESULT_OK,
    JS_EVAL_RESULT_EXCEPTION,
    JS_EVAL_RESULT_TIMEOUT,
    JS_EVAL_RESULT_RUNTIME_STOPPED,
} js_eval_result_status_t;

typedef struct {
    const char *source;
    size_t source_size;
    js_eval_mode_t mode;
    bool await_result;
    uint32_t timeout_ms;
    size_t max_result_size;
} js_eval_options_t;

typedef struct {
    js_eval_result_status_t status;
    bool async;
    bool truncated;
    char *type;
    char *value_json;
    char *display;
    char *error_name;
    char *error_message;
    char *error_stack;
} js_eval_result_t;

/* Source is copied. Completion is consumed by receive; destroy cancels safely. */
esp_err_t js_runtime_eval_submit(js_runtime_t *runtime,
                                 const js_eval_options_t *options,
                                 js_eval_request_t **out_request);
esp_err_t js_runtime_eval_receive(js_eval_request_t *request,
                                  js_eval_result_t *out_result,
                                  TickType_t wait_ticks);
void js_runtime_eval_request_destroy(js_eval_request_t *request);
void js_runtime_eval_result_free(js_eval_result_t *result);

#ifdef __cplusplus
}
#endif
