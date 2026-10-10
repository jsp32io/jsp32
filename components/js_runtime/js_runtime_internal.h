#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "js_runtime.h"

typedef struct js_timer js_timer_t;
typedef struct js_listener js_listener_t;
typedef struct js_event_handler_entry js_event_handler_entry_t;
typedef struct js_eval_request js_eval_request_t;

typedef struct {
    js_runtime_module_init_fn init;
    js_runtime_module_deinit_fn deinit;
    void *opaque;
} js_runtime_module_t;

#define JS_RUNTIME_MAX_MODULES 12

typedef struct {
    int64_t deadline_us;
    int64_t last_yield_us;
    bool active;
} js_execution_budget_t;

struct js_runtime {
    QueueHandle_t queue;
    SemaphoreHandle_t startup_ready;
    TaskHandle_t task;
    JSRuntime *rt;
    JSContext *ctx;
    app_filesystem_t *filesystem;
    JSValue entry_promise;
    const char *script;
    const char *filename;
    size_t script_size;
    size_t file_module_count;
    size_t file_module_source_bytes;
    js_timer_t *timers;
    js_timer_t *active_timer;
    js_listener_t *listeners;
    js_event_handler_entry_t *handlers;
    js_execution_budget_t budget;
    portMUX_TYPE stats_lock;
    uint32_t dropped_events;
    uint32_t active_posters;
    uint32_t next_timer_id;
    esp_err_t startup_result;
    js_runtime_module_t modules[JS_RUNTIME_MAX_MODULES];
    size_t module_count;
    size_t initialized_module_count;
    bool accepting_events;
    bool stopping;
    SemaphoreHandle_t console_lock;
    StaticSemaphore_t console_lock_storage;
    js_console_subscription_t *console_subscriptions;
    char *console_history[CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH];
    size_t console_history_start;
    size_t console_history_count;
    bool accepting_console;
    js_eval_request_t *pending_evals;
    js_request_table_t *request_tables;
};

void js_runtime_log_exception(JSContext *ctx);
void js_runtime_budget_begin(js_runtime_t *runtime);
void js_runtime_budget_end(js_runtime_t *runtime);
bool js_runtime_drain_jobs(js_runtime_t *runtime, unsigned int limit);
void js_modules_init(js_runtime_t *runtime);

void js_console_prepare(js_runtime_t *runtime);
esp_err_t js_console_init(js_runtime_t *runtime);
void js_console_close(js_runtime_t *runtime);
void js_console_publish(js_runtime_t *runtime, const char *line);

esp_err_t js_eval_init(js_runtime_t *runtime);
void js_eval_process_pending(js_runtime_t *runtime);
TickType_t js_eval_next_wait(js_runtime_t *runtime);
void js_eval_deinit(js_runtime_t *runtime);

esp_err_t js_event_init(js_runtime_t *runtime);
void js_event_deinit(js_runtime_t *runtime);
bool js_event_dispatch(js_runtime_t *runtime, js_event_t *event);
void js_event_stop_accepting(js_runtime_t *runtime);

esp_err_t js_listener_init(js_runtime_t *runtime);
void js_listener_deinit(js_runtime_t *runtime);

esp_err_t js_timer_init(js_runtime_t *runtime);
void js_timer_deinit(js_runtime_t *runtime);
TickType_t js_timer_next_wait(js_runtime_t *runtime);
unsigned int js_timer_dispatch_expired(js_runtime_t *runtime,
                                       unsigned int limit);
