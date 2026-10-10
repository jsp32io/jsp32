#include "js_event.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/queue.h"
#include "js_listener.h"
#include "js_runtime.h"
#include "js_runtime_internal.h"

struct js_event_handler_entry {
    uint16_t type;
    js_event_handler_fn handler;
    void *opaque;
    js_event_handler_entry_t *next;
};

static void increment_dropped(js_runtime_t *runtime, bool in_isr)
{
    if (in_isr) {
        portENTER_CRITICAL_ISR(&runtime->stats_lock);
        ++runtime->dropped_events;
        portEXIT_CRITICAL_ISR(&runtime->stats_lock);
    } else {
        portENTER_CRITICAL(&runtime->stats_lock);
        ++runtime->dropped_events;
        portEXIT_CRITICAL(&runtime->stats_lock);
    }
}

static bool begin_post(js_runtime_t *runtime, bool in_isr)
{
    bool accepted;
    if (in_isr) {
        portENTER_CRITICAL_ISR(&runtime->stats_lock);
        accepted = runtime->accepting_events;
        if (accepted) {
            ++runtime->active_posters;
        }
        portEXIT_CRITICAL_ISR(&runtime->stats_lock);
    } else {
        portENTER_CRITICAL(&runtime->stats_lock);
        accepted = runtime->accepting_events;
        if (accepted) {
            ++runtime->active_posters;
        }
        portEXIT_CRITICAL(&runtime->stats_lock);
    }
    return accepted;
}

static void end_post(js_runtime_t *runtime, bool in_isr)
{
    if (in_isr) {
        portENTER_CRITICAL_ISR(&runtime->stats_lock);
        --runtime->active_posters;
        portEXIT_CRITICAL_ISR(&runtime->stats_lock);
    } else {
        portENTER_CRITICAL(&runtime->stats_lock);
        --runtime->active_posters;
        portEXIT_CRITICAL(&runtime->stats_lock);
    }
}

void js_event_release(js_event_t *event)
{
    if (event && event->payload && event->free_payload) {
        event->free_payload(event->payload);
    }
    if (event) {
        event->payload = NULL;
        event->payload_size = 0;
        event->free_payload = NULL;
    }
}

esp_err_t js_event_post(js_runtime_t *runtime, const js_event_t *event,
                        TickType_t wait_ticks)
{
    if (!event) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!runtime || !begin_post(runtime, false)) {
        js_event_t owned = *event;
        js_event_release(&owned);
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t sent = xQueueSend(runtime->queue, event, wait_ticks);
    end_post(runtime, false);
    if (sent != pdTRUE) {
        js_event_t owned = *event;
        js_event_release(&owned);
        increment_dropped(runtime, false);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t js_event_post_from_isr(js_runtime_t *runtime,
                                 const js_event_t *event,
                                 BaseType_t *higher_priority_task_woken)
{
    if (!runtime || !event || event->payload || event->payload_size != 0 ||
        event->free_payload) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!begin_post(runtime, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    BaseType_t sent = xQueueSendFromISR(runtime->queue, event,
                                       higher_priority_task_woken);
    end_post(runtime, true);
    if (sent != pdTRUE) {
        increment_dropped(runtime, true);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

void js_event_stop_accepting(js_runtime_t *runtime)
{
    for (;;) {
        portENTER_CRITICAL(&runtime->stats_lock);
        runtime->accepting_events = false;
        uint32_t active = runtime->active_posters;
        portEXIT_CRITICAL(&runtime->stats_lock);
        if (active == 0) {
            return;
        }
        js_event_t queued;
        while (xQueueReceive(runtime->queue, &queued, 0) == pdTRUE) {
            js_event_release(&queued);
        }
        vTaskDelay(1);
    }
}

esp_err_t js_event_register_handler(js_runtime_t *runtime, uint16_t type,
                                    js_event_handler_fn handler, void *opaque)
{
    if (!js_runtime_is_owner(runtime) || !handler || type == JS_EVENT_STOP) {
        return ESP_ERR_INVALID_ARG;
    }
    for (js_event_handler_entry_t *item = runtime->handlers; item;
         item = item->next) {
        if (item->type == type) {
            return ESP_ERR_INVALID_STATE;
        }
    }

    js_event_handler_entry_t *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return ESP_ERR_NO_MEM;
    }
    entry->type = type;
    entry->handler = handler;
    entry->opaque = opaque;
    entry->next = runtime->handlers;
    runtime->handlers = entry;
    return ESP_OK;
}

esp_err_t js_event_unregister_handler(js_runtime_t *runtime, uint16_t type)
{
    if (!js_runtime_is_owner(runtime)) {
        return ESP_ERR_INVALID_STATE;
    }
    js_event_handler_entry_t **link = &runtime->handlers;
    while (*link) {
        js_event_handler_entry_t *entry = *link;
        if (entry->type == type) {
            *link = entry->next;
            free(entry);
            return ESP_OK;
        }
        link = &entry->next;
    }
    return ESP_ERR_NOT_FOUND;
}

bool js_event_dispatch(js_runtime_t *runtime, js_event_t *event)
{
    if (event->type == JS_EVENT_STOP) {
        return false;
    }
    for (js_event_handler_entry_t *item = runtime->handlers; item;
         item = item->next) {
        if (item->type == event->type) {
            item->handler(runtime, event, item->opaque);
            break;
        }
    }
    return true;
}

static void user_event_handler(js_runtime_t *runtime,
                               const js_event_t *event, void *opaque)
{
    (void)opaque;
    const char *message = event->payload ? event->payload : "";
    JSValue value = JS_NewStringLen(runtime->ctx, message,
                                    event->payload ? event->payload_size : 0);
    if (JS_IsException(value)) {
        js_runtime_log_exception(runtime->ctx);
        return;
    }
    js_listener_emit(runtime, JS_EVENT_USER, 0, 0, value);
    JS_FreeValue(runtime->ctx, value);
}

static JSValue js_events_on(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    js_runtime_t *runtime = JS_GetContextOpaque(ctx);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name) {
        return JS_EXCEPTION;
    }
    bool valid_name = strcmp(name, "message") == 0;
    JS_FreeCString(ctx, name);
    if (!valid_name) {
        return JS_ThrowRangeError(ctx, "unknown event name");
    }
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "listener must be a function");
    }
    if (js_listener_add(runtime, JS_EVENT_USER, 0, 0, argv[1]) != ESP_OK) {
        return JS_ThrowOutOfMemory(ctx);
    }
    return JS_UNDEFINED;
}

static JSValue js_events_off(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    js_runtime_t *runtime = JS_GetContextOpaque(ctx);
    const char *name = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!name) {
        return JS_EXCEPTION;
    }
    bool valid_name = strcmp(name, "message") == 0;
    JS_FreeCString(ctx, name);
    if (!valid_name) {
        return JS_ThrowRangeError(ctx, "unknown event name");
    }
    JSValueConst callback = argc > 1 ? argv[1] : JS_UNDEFINED;
    size_t removed = js_listener_remove(runtime, JS_EVENT_USER, 0, 0,
                                        callback);
    return JS_NewBool(ctx, removed != 0);
}

static JSValue js_events_post(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void)this_value;
    js_runtime_t *runtime = JS_GetContextOpaque(ctx);
    size_t length = 0;
    const char *message = argc > 0
        ? JS_ToCStringLen(ctx, &length, argv[0]) : NULL;
    if (!message) {
        return JS_EXCEPTION;
    }
    char *copy = malloc(length + 1);
    if (!copy) {
        JS_FreeCString(ctx, message);
        return JS_ThrowOutOfMemory(ctx);
    }
    memcpy(copy, message, length);
    copy[length] = '\0';
    JS_FreeCString(ctx, message);

    js_event_t event = {
        .type = JS_EVENT_USER,
        .payload = copy,
        .payload_size = length,
        .free_payload = free,
    };
    esp_err_t result = js_event_post(runtime, &event, 0);
    return JS_NewBool(ctx, result == ESP_OK);
}

static JSValue js_events_stats(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    js_runtime_t *runtime = JS_GetContextOpaque(ctx);
    JSValue stats = JS_NewObject(ctx);
    if (JS_IsException(stats)) {
        return stats;
    }
    JS_SetPropertyStr(ctx, stats, "queued",
        JS_NewUint32(ctx, uxQueueMessagesWaiting(runtime->queue)));
    JS_SetPropertyStr(ctx, stats, "dropped",
        JS_NewUint32(ctx, js_runtime_dropped_events(runtime)));
    return stats;
}

static esp_err_t install_method(JSContext *ctx, JSValue object,
                                const char *name, JSCFunction *function,
                                int argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);
    if (JS_IsException(value)) {
        return ESP_ERR_NO_MEM;
    }
    return JS_SetPropertyStr(ctx, object, name, value) < 0
        ? ESP_FAIL : ESP_OK;
}

esp_err_t js_event_init(js_runtime_t *runtime)
{
    runtime->handlers = NULL;
    esp_err_t err = js_event_register_handler(runtime, JS_EVENT_USER,
                                               user_event_handler, NULL);
    if (err != ESP_OK) {
        return err;
    }

    JSContext *ctx = runtime->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue events = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(events)) {
        JS_FreeValue(ctx, events);
        JS_FreeValue(ctx, global);
        return ESP_ERR_NO_MEM;
    }
    if (install_method(ctx, events, "on", js_events_on, 2) != ESP_OK ||
        install_method(ctx, events, "off", js_events_off, 2) != ESP_OK ||
        install_method(ctx, events, "post", js_events_post, 1) != ESP_OK ||
        install_method(ctx, events, "stats", js_events_stats, 0) != ESP_OK) {
        JS_FreeValue(ctx, events);
        JS_FreeValue(ctx, global);
        return ESP_FAIL;
    }
    if (JS_SetPropertyStr(ctx, global, "events", events) < 0) {
        JS_FreeValue(ctx, global);
        return ESP_FAIL;
    }
    JS_FreeValue(ctx, global);
    return ESP_OK;
}

void js_event_deinit(js_runtime_t *runtime)
{
    js_event_handler_entry_t *handler = runtime->handlers;
    runtime->handlers = NULL;
    while (handler) {
        js_event_handler_entry_t *next = handler->next;
        free(handler);
        handler = next;
    }
}
