#include "js_runtime.h"

#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "js_runtime_internal.h"
#include "sdkconfig.h"

static const char *TAG = "js_runtime";

static js_runtime_t s_runtime = {
    .stats_lock = portMUX_INITIALIZER_UNLOCKED,
};
static bool s_runtime_started;

esp_err_t js_runtime_track_requests(js_runtime_t *runtime, js_request_table_t *table)
{
    if (!runtime || !table || !js_runtime_is_owner(runtime) || table->ctx != runtime->ctx)
        return ESP_ERR_INVALID_STATE;
    for (js_request_table_t *item = runtime->request_tables; item; item = item->next_table) {
        if (item == table) return ESP_OK;
    }
    table->next_table = runtime->request_tables;
    runtime->request_tables = table;
    return ESP_OK;
}

static JSValue request_error(JSContext *ctx, const char *name, const char *message)
{
    JSValue error = JS_NewError(ctx);
    if (!JS_IsException(error)) {
        if (JS_SetPropertyStr(ctx, error, "name", JS_NewString(ctx, name)) < 0 ||
            JS_SetPropertyStr(ctx, error, "message", JS_NewString(ctx, message)) < 0) {
            JS_FreeValue(ctx, error);
            error = JS_EXCEPTION;
        }
    }
    return JS_IsException(error) ? JS_GetException(ctx) : error;
}

static TickType_t request_next_wait(js_runtime_t *runtime)
{
    int64_t deadline = 0;
    for (js_request_table_t *table = runtime->request_tables; table; table = table->next_table) {
        int64_t candidate = js_request_next_deadline(table);
        if (candidate && (!deadline || candidate < deadline)) deadline = candidate;
    }
    if (!deadline) return portMAX_DELAY;
    int64_t remaining = deadline - esp_timer_get_time();
    if (remaining <= 0) return 0;
    uint64_t tick_us = (uint64_t)portTICK_PERIOD_MS * 1000;
    uint64_t ticks = ((uint64_t)remaining + tick_us - 1) / tick_us;
    return ticks >= portMAX_DELAY ? portMAX_DELAY - 1 : (TickType_t)ticks;
}

static void process_request_deadlines(js_runtime_t *runtime)
{
    int64_t now = esp_timer_get_time();
    for (js_request_table_t *table = runtime->request_tables; table; table = table->next_table) {
        int64_t deadline = js_request_next_deadline(table);
        if (!deadline || deadline > now) continue;
        JSValue error = request_error(runtime->ctx, "TimeoutError", "request timed out");
        js_runtime_budget_begin(runtime);
        js_request_expire(table, now, error);
        js_runtime_budget_end(runtime);
        JS_FreeValue(runtime->ctx, error);
    }
}

static void close_request_tables(js_runtime_t *runtime)
{
    if (!runtime->request_tables) return;
    JSValue error = request_error(runtime->ctx, "Error", "runtime stopped");
    while (runtime->request_tables) {
        js_request_table_t *table = runtime->request_tables;
        runtime->request_tables = table->next_table;
        table->next_table = NULL;
        js_request_table_close(table, error);
    }
    JS_FreeValue(runtime->ctx, error);
}

static int interrupt_handler(JSRuntime *rt, void *opaque)
{
    (void)rt;
    js_runtime_t *runtime = opaque;
    if (!runtime->budget.active) {
        return 0;
    }

    int64_t now = esp_timer_get_time();
    if (now >= runtime->budget.deadline_us) {
        return 1;
    }
    if (now - runtime->budget.last_yield_us >= 10000) {
        taskYIELD();
        runtime->budget.last_yield_us = esp_timer_get_time();
    }
    return 0;
}

void js_runtime_budget_begin(js_runtime_t *runtime)
{
    int64_t now = esp_timer_get_time();
    runtime->budget.deadline_us =
        now + (int64_t)CONFIG_JS_RUNTIME_CALL_BUDGET_MS * 1000;
    runtime->budget.last_yield_us = now;
    runtime->budget.active = true;
}

void js_runtime_budget_end(js_runtime_t *runtime)
{
    runtime->budget.active = false;
}

void js_runtime_log_exception(JSContext *ctx)
{
    if (!ctx) {
        ESP_LOGE(TAG, "JavaScript exception without a context");
        return;
    }

    JSValue error = JS_GetException(ctx);
    const char *message = JS_ToCString(ctx, error);
    ESP_LOGE(TAG, "%s", message ? message : "JavaScript exception");
    JS_FreeCString(ctx, message);

    if (JS_IsError(ctx, error)) {
        JSValue stack = JS_GetPropertyStr(ctx, error, "stack");
        if (!JS_IsException(stack) && !JS_IsUndefined(stack)) {
            const char *trace = JS_ToCString(ctx, stack);
            if (trace) {
                ESP_LOGE(TAG, "%s", trace);
                JS_FreeCString(ctx, trace);
            }
        }
        JS_FreeValue(ctx, stack);
    }
    JS_FreeValue(ctx, error);
}

bool js_runtime_call(js_runtime_t *runtime, JSValueConst callback,
                     JSValueConst this_value, int argc,
                     JSValueConst *argv)
{
    if (!js_runtime_is_owner(runtime) || !runtime->ctx) {
        return false;
    }
    js_runtime_budget_begin(runtime);
    JSValue result = JS_Call(runtime->ctx, callback, this_value, argc, argv);
    js_runtime_budget_end(runtime);
    if (JS_IsException(result)) {
        js_runtime_log_exception(runtime->ctx);
        return false;
    }
    JS_FreeValue(runtime->ctx, result);
    return true;
}

bool js_runtime_drain_jobs(js_runtime_t *runtime, unsigned int limit)
{
    for (unsigned int i = 0; i < limit && JS_IsJobPending(runtime->rt); ++i) {
        JSContext *job_ctx = NULL;
        js_runtime_budget_begin(runtime);
        int result = JS_ExecutePendingJob(runtime->rt, &job_ctx);
        js_runtime_budget_end(runtime);
        if (result < 0) {
            js_runtime_log_exception(job_ctx ? job_ctx : runtime->ctx);
            break;
        }
    }
    return JS_IsJobPending(runtime->rt);
}

static void check_entry_promise(js_runtime_t *runtime)
{
    if (JS_IsUndefined(runtime->entry_promise)) return;
    int state = JS_PromiseState(runtime->ctx, runtime->entry_promise);
    if (state == JS_PROMISE_PENDING) return;
    if (state == JS_PROMISE_REJECTED) {
        JSValue reason = JS_PromiseResult(runtime->ctx, runtime->entry_promise);
        JS_Throw(runtime->ctx, reason);
        js_runtime_log_exception(runtime->ctx);
    }
    JS_FreeValue(runtime->ctx, runtime->entry_promise);
    runtime->entry_promise = JS_UNDEFINED;
}

static void evaluate_entry_script(js_runtime_t *runtime)
{
    js_runtime_budget_begin(runtime);
    JSValue result = JS_Eval(runtime->ctx, runtime->script,
                             runtime->script_size, runtime->filename,
                             JS_EVAL_TYPE_MODULE);
    js_runtime_budget_end(runtime);
    if (JS_IsException(result)) {
        js_runtime_log_exception(runtime->ctx);
    } else {
        runtime->entry_promise = result;
        check_entry_promise(runtime);
    }
}

static void release_queued_events(js_runtime_t *runtime)
{
    js_event_t event;
    while (xQueueReceive(runtime->queue, &event, 0) == pdTRUE) {
        js_event_release(&event);
    }
}

static void runtime_task(void *arg)
{
    js_runtime_t *runtime = arg;
    bool startup_reported = false;
    runtime->task = xTaskGetCurrentTaskHandle();

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "PSRAM is required for the QuickJS heap");
        goto cleanup;
    }
    ESP_LOGI(TAG, "PSRAM: %u bytes", (unsigned)esp_psram_get_size());

    runtime->rt = JS_NewRuntime();
    if (!runtime->rt) {
        ESP_LOGE(TAG, "Cannot create QuickJS runtime");
        goto cleanup;
    }
    JS_SetMemoryLimit(runtime->rt,
                      (size_t)CONFIG_JS_RUNTIME_MEMORY_LIMIT_KB * 1024);
    JS_SetMaxStackSize(runtime->rt,
                       (size_t)CONFIG_JS_RUNTIME_STACK_LIMIT_KB * 1024);
    JS_SetInterruptHandler(runtime->rt, interrupt_handler, runtime);
    js_modules_init(runtime);

    runtime->ctx = JS_NewContext(runtime->rt);
    if (!runtime->ctx) {
        ESP_LOGE(TAG, "Cannot create QuickJS context");
        goto cleanup;
    }
    JS_SetContextOpaque(runtime->ctx, runtime);

    if (js_console_init(runtime) != ESP_OK ||
        js_listener_init(runtime) != ESP_OK ||
        js_timer_init(runtime) != ESP_OK ||
        js_event_init(runtime) != ESP_OK ||
        js_eval_init(runtime) != ESP_OK) {
        ESP_LOGE(TAG, "Cannot install JavaScript host APIs");
        goto cleanup;
    }

    for (size_t i = 0; i < runtime->module_count; ++i) {
        esp_err_t err = runtime->modules[i].init(
            runtime, runtime->ctx, runtime->modules[i].opaque);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Module %u initialization failed: %s",
                     (unsigned)i, esp_err_to_name(err));
            goto cleanup;
        }
        ++runtime->initialized_module_count;
    }

    runtime->startup_result = ESP_OK;
    xSemaphoreGive(runtime->startup_ready);
    startup_reported = true;

    evaluate_entry_script(runtime);

    bool jobs_pending = js_runtime_drain_jobs(
        runtime, CONFIG_JS_RUNTIME_MAX_JOBS_PER_TURN);
    check_entry_promise(runtime);
    js_eval_process_pending(runtime);
    while (!runtime->stopping) {
        process_request_deadlines(runtime);
        jobs_pending = JS_IsJobPending(runtime->rt);
        if (jobs_pending) {
            jobs_pending = js_runtime_drain_jobs(
                runtime, CONFIG_JS_RUNTIME_MAX_JOBS_PER_TURN);
            check_entry_promise(runtime);
            js_eval_process_pending(runtime);
        }
        TickType_t wait = jobs_pending ? 0 : js_timer_next_wait(runtime);
        TickType_t eval_wait = js_eval_next_wait(runtime);
        if (eval_wait < wait) {
            wait = eval_wait;
        }
        TickType_t request_wait = request_next_wait(runtime);
        if (request_wait < wait) wait = request_wait;
        js_event_t event;
        if (xQueueReceive(runtime->queue, &event, wait) == pdTRUE) {
            if (!js_event_dispatch(runtime, &event)) {
                runtime->stopping = true;
            }
            js_event_release(&event);
            jobs_pending = js_runtime_drain_jobs(
                runtime, CONFIG_JS_RUNTIME_MAX_JOBS_PER_TURN);
            check_entry_promise(runtime);
            js_eval_process_pending(runtime);
        }

        if (!runtime->stopping) {
            js_timer_dispatch_expired(
                runtime, CONFIG_JS_RUNTIME_MAX_TIMERS_PER_TURN);
            js_eval_process_pending(runtime);
            jobs_pending = JS_IsJobPending(runtime->rt);
        }
    }

cleanup:
    js_console_close(runtime);
    js_event_stop_accepting(runtime);
    release_queued_events(runtime);
    while (runtime->initialized_module_count > 0) {
        js_runtime_module_t *module =
            &runtime->modules[--runtime->initialized_module_count];
        if (module->deinit) {
            module->deinit(runtime, runtime->ctx, module->opaque);
        }
    }
    close_request_tables(runtime);
    js_timer_deinit(runtime);
    js_listener_deinit(runtime);
    js_eval_deinit(runtime);
    js_event_deinit(runtime);
    if (runtime->ctx) {
        JS_FreeValue(runtime->ctx, runtime->entry_promise);
        runtime->entry_promise = JS_UNDEFINED;
    }
    if (runtime->ctx) {
        JS_FreeContext(runtime->ctx);
        runtime->ctx = NULL;
    }
    if (runtime->rt) {
        JS_FreeRuntime(runtime->rt);
        runtime->rt = NULL;
    }
    vQueueDelete(runtime->queue);
    runtime->queue = NULL;
    runtime->task = NULL;
    if (!startup_reported) {
        runtime->startup_result = ESP_FAIL;
        xSemaphoreGive(runtime->startup_ready);
    }
    ESP_LOGI(TAG, "JavaScript EventLoop stopped");
    vTaskDelete(NULL);
}

esp_err_t js_runtime_register_module(js_runtime_module_init_fn init,
                                     js_runtime_module_deinit_fn deinit,
                                     void *opaque)
{
    if (!init) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_runtime.module_count >= JS_RUNTIME_MAX_MODULES) {
        return ESP_ERR_NO_MEM;
    }
    s_runtime.modules[s_runtime.module_count++] = (js_runtime_module_t) {
        .init = init,
        .deinit = deinit,
        .opaque = opaque,
    };
    return ESP_OK;
}

esp_err_t js_runtime_start(const char *script, size_t script_size,
                           const char *filename, app_filesystem_t *filesystem,
                           js_runtime_t **out_runtime)
{
    if (!script || script_size == 0 || !filename || !filesystem || !out_runtime) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_runtime_started) {
        return ESP_ERR_INVALID_STATE;
    }

    s_runtime.queue = xQueueCreate(CONFIG_JS_EVENT_QUEUE_LENGTH,
                                   sizeof(js_event_t));
    if (!s_runtime.queue) {
        return ESP_ERR_NO_MEM;
    }
    s_runtime.startup_ready = xSemaphoreCreateBinary();
    if (!s_runtime.startup_ready) {
        vQueueDelete(s_runtime.queue);
        s_runtime.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_runtime.script = script;
    s_runtime.script_size = script_size;
    s_runtime.filename = filename;
    s_runtime.filesystem = filesystem;
    s_runtime.entry_promise = JS_UNDEFINED;
    s_runtime.file_module_count = 0;
    s_runtime.file_module_source_bytes = 0;
    s_runtime.next_timer_id = 1;
    s_runtime.accepting_events = true;
    js_console_prepare(&s_runtime);
    s_runtime_started = true;

    BaseType_t created = xTaskCreate(
        runtime_task, "js_event_loop", CONFIG_JS_RUNTIME_TASK_STACK_SIZE,
        &s_runtime, CONFIG_JS_RUNTIME_TASK_PRIORITY, &s_runtime.task);
    if (created != pdPASS) {
        s_runtime.accepting_events = false;
        vSemaphoreDelete(s_runtime.console_lock);
        s_runtime.console_lock = NULL;
        vSemaphoreDelete(s_runtime.startup_ready);
        s_runtime.startup_ready = NULL;
        vQueueDelete(s_runtime.queue);
        s_runtime.queue = NULL;
        s_runtime_started = false;
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_runtime.startup_ready, portMAX_DELAY);
    vSemaphoreDelete(s_runtime.startup_ready);
    s_runtime.startup_ready = NULL;
    if (s_runtime.startup_result != ESP_OK) {
        return s_runtime.startup_result;
    }

    *out_runtime = &s_runtime;
    return ESP_OK;
}

esp_err_t js_runtime_stop(js_runtime_t *runtime, TickType_t wait_ticks)
{
    if (runtime != &s_runtime) {
        return ESP_ERR_INVALID_STATE;
    }
    js_event_t event = {.type = JS_EVENT_STOP};
    return js_event_post(runtime, &event, wait_ticks);
}

bool js_runtime_is_owner(const js_runtime_t *runtime)
{
    return runtime && runtime->task == xTaskGetCurrentTaskHandle();
}

JSContext *js_runtime_context(js_runtime_t *runtime)
{
    return js_runtime_is_owner(runtime) ? runtime->ctx : NULL;
}

uint32_t js_runtime_dropped_events(js_runtime_t *runtime)
{
    if (!runtime) {
        return 0;
    }
    portENTER_CRITICAL(&runtime->stats_lock);
    uint32_t result = runtime->dropped_events;
    portEXIT_CRITICAL(&runtime->stats_lock);
    return result;
}
