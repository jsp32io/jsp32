#include <limits.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "js_runtime.h"
#include "js_runtime_internal.h"
#include "sdkconfig.h"

struct js_timer {
    uint32_t id;
    int64_t deadline_us;
    int64_t interval_us;
    JSValue callback;
    JSValue *args;
    int argc;
    bool cancelled;
    js_timer_t *next;
};

static void timer_free(js_runtime_t *runtime, js_timer_t *timer)
{
    JS_FreeValue(runtime->ctx, timer->callback);
    for (int i = 0; i < timer->argc; ++i) {
        JS_FreeValue(runtime->ctx, timer->args[i]);
    }
    free(timer->args);
    free(timer);
}

static void timer_insert(js_runtime_t *runtime, js_timer_t *timer)
{
    js_timer_t **link = &runtime->timers;
    while (*link && (*link)->deadline_us <= timer->deadline_us) {
        link = &(*link)->next;
    }
    timer->next = *link;
    *link = timer;
}

static uint32_t next_timer_id(js_runtime_t *runtime)
{
    uint32_t id = runtime->next_timer_id++;
    if (runtime->next_timer_id == 0) {
        runtime->next_timer_id = 1;
    }
    return id ? id : runtime->next_timer_id++;
}

static JSValue set_timer(JSContext *ctx, int argc, JSValueConst *argv,
                         bool repeating)
{
    js_runtime_t *runtime = JS_GetContextOpaque(ctx);
    if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "callback must be a function");
    }

    int64_t delay_ms = 0;
    if (argc > 1 && JS_ToInt64(ctx, &delay_ms, argv[1])) {
        return JS_EXCEPTION;
    }
    if (delay_ms < 0) {
        delay_ms = 0;
    } else if (delay_ms > INT32_MAX) {
        delay_ms = INT32_MAX;
    }
    if (repeating && delay_ms < 1) {
        delay_ms = 1;
    }

    js_timer_t *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        return JS_ThrowOutOfMemory(ctx);
    }
    timer->argc = argc > 2 ? argc - 2 : 0;
    if (timer->argc) {
        timer->args = calloc((size_t)timer->argc, sizeof(*timer->args));
        if (!timer->args) {
            free(timer);
            return JS_ThrowOutOfMemory(ctx);
        }
        for (int i = 0; i < timer->argc; ++i) {
            timer->args[i] = JS_DupValue(ctx, argv[i + 2]);
        }
    }
    timer->id = next_timer_id(runtime);
    timer->deadline_us = esp_timer_get_time() + delay_ms * 1000;
    timer->interval_us = repeating ? delay_ms * 1000 : 0;
    timer->callback = JS_DupValue(ctx, argv[0]);
    timer_insert(runtime, timer);
    return JS_NewUint32(ctx, timer->id);
}

static JSValue js_set_timeout(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void)this_value;
    return set_timer(ctx, argc, argv, false);
}

static JSValue js_set_interval(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void)this_value;
    return set_timer(ctx, argc, argv, true);
}

static bool clear_timer(js_runtime_t *runtime, uint32_t id)
{
    if (runtime->active_timer && runtime->active_timer->id == id) {
        runtime->active_timer->cancelled = true;
        return true;
    }
    js_timer_t **link = &runtime->timers;
    while (*link) {
        js_timer_t *timer = *link;
        if (timer->id == id) {
            *link = timer->next;
            timer_free(runtime, timer);
            return true;
        }
        link = &timer->next;
    }
    return false;
}

static JSValue js_clear_timer(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void)this_value;
    js_runtime_t *runtime = JS_GetContextOpaque(ctx);
    uint32_t id = 0;
    if (argc > 0 && JS_ToUint32(ctx, &id, argv[0])) {
        return JS_EXCEPTION;
    }
    clear_timer(runtime, id);
    return JS_UNDEFINED;
}

static JSValue js_monotonic_ms(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    return JS_NewFloat64(ctx, (double)esp_timer_get_time() / 1000.0);
}

static esp_err_t install_global_function(JSContext *ctx, JSValue global,
                                         const char *name,
                                         JSCFunction *function, int argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);
    if (JS_IsException(value) ||
        JS_SetPropertyStr(ctx, global, name, value) < 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t js_timer_init(js_runtime_t *runtime)
{
    runtime->timers = NULL;
    runtime->active_timer = NULL;
    JSValue global = JS_GetGlobalObject(runtime->ctx);
    if (JS_IsException(global)) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t result = ESP_OK;
    if (install_global_function(runtime->ctx, global, "setTimeout",
                                js_set_timeout, 2) != ESP_OK ||
        install_global_function(runtime->ctx, global, "clearTimeout",
                                js_clear_timer, 1) != ESP_OK ||
        install_global_function(runtime->ctx, global, "setInterval",
                                js_set_interval, 2) != ESP_OK ||
        install_global_function(runtime->ctx, global, "clearInterval",
                                js_clear_timer, 1) != ESP_OK ||
        install_global_function(runtime->ctx, global, "monotonicMs",
                                js_monotonic_ms, 0) != ESP_OK) {
        result = ESP_FAIL;
    }
    JS_FreeValue(runtime->ctx, global);
    return result;
}

TickType_t js_timer_next_wait(js_runtime_t *runtime)
{
    if (!runtime->timers) {
        return portMAX_DELAY;
    }
    int64_t remaining_us = runtime->timers->deadline_us - esp_timer_get_time();
    if (remaining_us <= 0) {
        return 0;
    }
    uint64_t wait_ms = ((uint64_t)remaining_us + 999) / 1000;
    uint64_t ticks = (wait_ms * configTICK_RATE_HZ + 999) / 1000;
    if (ticks == 0) {
        ticks = 1;
    }
    if (ticks >= portMAX_DELAY) {
        ticks = portMAX_DELAY - 1;
    }
    return (TickType_t)ticks;
}

unsigned int js_timer_dispatch_expired(js_runtime_t *runtime,
                                       unsigned int limit)
{
    unsigned int dispatched = 0;
    while (runtime->timers && dispatched < limit) {
        int64_t now = esp_timer_get_time();
        if (runtime->timers->deadline_us > now) {
            break;
        }

        js_timer_t *timer = runtime->timers;
        runtime->timers = timer->next;
        timer->next = NULL;
        runtime->active_timer = timer;
        js_runtime_call(runtime, timer->callback, JS_UNDEFINED,
                        timer->argc, timer->args);
        runtime->active_timer = NULL;
        ++dispatched;

        if (timer->interval_us > 0 && !timer->cancelled) {
            timer->deadline_us += timer->interval_us;
            now = esp_timer_get_time();
            if (timer->deadline_us <= now) {
                timer->deadline_us = now + timer->interval_us;
            }
            timer_insert(runtime, timer);
        } else {
            timer_free(runtime, timer);
        }

        if (js_runtime_drain_jobs(
                runtime, CONFIG_JS_RUNTIME_MAX_JOBS_PER_TURN)) {
            break;
        }
    }
    return dispatched;
}

void js_timer_deinit(js_runtime_t *runtime)
{
    if (runtime->active_timer) {
        timer_free(runtime, runtime->active_timer);
        runtime->active_timer = NULL;
    }
    js_timer_t *timer = runtime->timers;
    runtime->timers = NULL;
    while (timer) {
        js_timer_t *next = timer->next;
        timer_free(runtime, timer);
        timer = next;
    }
}
