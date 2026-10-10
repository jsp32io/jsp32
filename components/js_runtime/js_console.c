#include "js_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/queue.h"
#include "js_runtime_internal.h"
#include "sdkconfig.h"

typedef struct {
    char *line;
    uint32_t dropped_before;
    bool closed;
} js_console_message_t;

struct js_console_subscription {
    js_runtime_t *runtime;
    QueueHandle_t queue;
    js_console_subscription_t *next;
    uint32_t pending_dropped;
    bool active;
};

static size_t utf8_sequence_length(unsigned char byte)
{
    if ((byte & 0x80) == 0) {
        return 1;
    }
    if ((byte & 0xe0) == 0xc0) {
        return 2;
    }
    if ((byte & 0xf0) == 0xe0) {
        return 3;
    }
    if ((byte & 0xf8) == 0xf0) {
        return 4;
    }
    return 1;
}

static void append_console_value(char *line, size_t *length,
                                 const char *value)
{
    while (*value && *length < JS_RUNTIME_CONSOLE_LINE_MAX - 1) {
        unsigned char byte = (unsigned char)*value;
        if (byte == '\r' || byte == '\n') {
            line[(*length)++] = ' ';
            ++value;
            continue;
        }
        size_t sequence_length = utf8_sequence_length(byte);
        if (*length + sequence_length >= JS_RUNTIME_CONSOLE_LINE_MAX) {
            break;
        }
        for (size_t i = 0; i < sequence_length && value[i]; ++i) {
            line[(*length)++] = value[i];
        }
        value += sequence_length;
    }
}

static JSValue js_console_log(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv)
{
    (void)this_value;
    char line[JS_RUNTIME_CONSOLE_LINE_MAX];
    size_t length = 0;
    for (int i = 0; i < argc; ++i) {
        const char *value = JS_ToCString(ctx, argv[i]);
        if (!value) {
            return JS_EXCEPTION;
        }
        printf("%s%s", i ? " " : "", value);
        if (i && length < sizeof(line) - 1) {
            line[length++] = ' ';
        }
        append_console_value(line, &length, value);
        JS_FreeCString(ctx, value);
    }
    putchar('\n');
    line[length] = '\0';
    js_console_publish(JS_GetContextOpaque(ctx), line);
    return JS_UNDEFINED;
}

void js_console_prepare(js_runtime_t *runtime)
{
    runtime->console_lock = xSemaphoreCreateMutexStatic(
        &runtime->console_lock_storage);
    runtime->console_subscriptions = NULL;
    runtime->console_history_start = 0;
    runtime->console_history_count = 0;
    runtime->accepting_console = true;
}

esp_err_t js_console_init(js_runtime_t *runtime)
{
    JSContext *ctx = runtime->ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue console = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(console)) {
        JS_FreeValue(ctx, console);
        JS_FreeValue(ctx, global);
        return ESP_ERR_NO_MEM;
    }

    JSValue log = JS_NewCFunction(ctx, js_console_log, "log", 1);
    if (JS_IsException(log)) {
        JS_FreeValue(ctx, console);
        JS_FreeValue(ctx, global);
        return ESP_ERR_NO_MEM;
    }
    if (JS_SetPropertyStr(ctx, console, "log", log) < 0) {
        JS_FreeValue(ctx, console);
        JS_FreeValue(ctx, global);
        return ESP_FAIL;
    }
    if (JS_SetPropertyStr(ctx, global, "console", console) < 0) {
        JS_FreeValue(ctx, global);
        return ESP_FAIL;
    }
    JS_FreeValue(ctx, global);
    return ESP_OK;
}

static void increment_dropped(js_console_subscription_t *subscription)
{
    if (subscription->pending_dropped != UINT32_MAX) {
        ++subscription->pending_dropped;
    }
}

void js_console_publish(js_runtime_t *runtime, const char *line)
{
    if (!runtime || !line || !runtime->console_lock) {
        return;
    }

    xSemaphoreTake(runtime->console_lock, portMAX_DELAY);
    if (!runtime->accepting_console) {
        xSemaphoreGive(runtime->console_lock);
        return;
    }

    char *history_line = strdup(line);
    if (history_line) {
        if (runtime->console_history_count ==
            CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH) {
            free(runtime->console_history[runtime->console_history_start]);
            runtime->console_history[runtime->console_history_start] = history_line;
            runtime->console_history_start =
                (runtime->console_history_start + 1) %
                CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH;
        } else {
            size_t index = (runtime->console_history_start +
                            runtime->console_history_count) %
                           CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH;
            runtime->console_history[index] = history_line;
            ++runtime->console_history_count;
        }
    }

    for (js_console_subscription_t *item = runtime->console_subscriptions;
         item; item = item->next) {
        js_console_message_t message = {
            .line = strdup(line),
            .dropped_before = item->pending_dropped,
        };
        if (!message.line ||
            xQueueSend(item->queue, &message, 0) != pdTRUE) {
            free(message.line);
            increment_dropped(item);
        } else {
            item->pending_dropped = 0;
        }
    }
    xSemaphoreGive(runtime->console_lock);
}

esp_err_t js_runtime_console_subscribe(js_runtime_t *runtime,
                                       js_console_subscription_t **out)
{
    if (!runtime || !out || !runtime->console_lock) {
        return ESP_ERR_INVALID_ARG;
    }
    js_console_subscription_t *subscription = calloc(1, sizeof(*subscription));
    if (!subscription) {
        return ESP_ERR_NO_MEM;
    }
    subscription->queue = xQueueCreate(CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH,
                                       sizeof(js_console_message_t));
    if (!subscription->queue) {
        free(subscription);
        return ESP_ERR_NO_MEM;
    }
    subscription->runtime = runtime;
    subscription->active = true;

    xSemaphoreTake(runtime->console_lock, portMAX_DELAY);
    if (!runtime->accepting_console) {
        xSemaphoreGive(runtime->console_lock);
        vQueueDelete(subscription->queue);
        free(subscription);
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < runtime->console_history_count; ++i) {
        size_t index = (runtime->console_history_start + i) %
                       CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH;
        js_console_message_t message = {
            .line = strdup(runtime->console_history[index]),
        };
        if (!message.line ||
            xQueueSend(subscription->queue, &message, 0) != pdTRUE) {
            free(message.line);
            increment_dropped(subscription);
        }
    }
    subscription->next = runtime->console_subscriptions;
    runtime->console_subscriptions = subscription;
    xSemaphoreGive(runtime->console_lock);
    *out = subscription;
    return ESP_OK;
}

esp_err_t js_runtime_console_receive(js_console_subscription_t *subscription,
                                     char *buffer, size_t buffer_size,
                                     uint32_t *out_dropped,
                                     TickType_t wait_ticks)
{
    if (!subscription || !buffer || buffer_size == 0 || !out_dropped ||
        !subscription->active) {
        return ESP_ERR_INVALID_ARG;
    }
    js_console_message_t message = {0};
    if (xQueueReceive(subscription->queue, &message, wait_ticks) != pdTRUE) {
        js_runtime_t *runtime = subscription->runtime;
        xSemaphoreTake(runtime->console_lock, portMAX_DELAY);
        uint32_t dropped = subscription->pending_dropped;
        subscription->pending_dropped = 0;
        xSemaphoreGive(runtime->console_lock);
        if (dropped > 0) {
            buffer[0] = '\0';
            *out_dropped = dropped;
            return ESP_ERR_NOT_FOUND;
        }
        return ESP_ERR_TIMEOUT;
    }
    if (message.closed) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(buffer, message.line, buffer_size);
    *out_dropped = message.dropped_before;
    free(message.line);
    return ESP_OK;
}

void js_runtime_console_unsubscribe(js_console_subscription_t *subscription)
{
    if (!subscription || !subscription->active) {
        return;
    }
    js_runtime_t *runtime = subscription->runtime;
    xSemaphoreTake(runtime->console_lock, portMAX_DELAY);
    js_console_subscription_t **link = &runtime->console_subscriptions;
    while (*link && *link != subscription) {
        link = &(*link)->next;
    }
    if (*link == subscription) {
        *link = subscription->next;
    }
    subscription->active = false;
    xSemaphoreGive(runtime->console_lock);

    js_console_message_t message;
    while (xQueueReceive(subscription->queue, &message, 0) == pdTRUE) {
        free(message.line);
    }
    vQueueDelete(subscription->queue);
    free(subscription);
}

void js_console_close(js_runtime_t *runtime)
{
    xSemaphoreTake(runtime->console_lock, portMAX_DELAY);
    runtime->accepting_console = false;
    for (size_t i = 0; i < runtime->console_history_count; ++i) {
        size_t index = (runtime->console_history_start + i) %
                       CONFIG_JS_RUNTIME_CONSOLE_QUEUE_LENGTH;
        free(runtime->console_history[index]);
        runtime->console_history[index] = NULL;
    }
    runtime->console_history_start = 0;
    runtime->console_history_count = 0;

    for (js_console_subscription_t *item = runtime->console_subscriptions;
         item; item = item->next) {
        js_console_message_t message;
        while (xQueueReceive(item->queue, &message, 0) == pdTRUE) {
            free(message.line);
        }
        message = (js_console_message_t){.closed = true};
        xQueueSend(item->queue, &message, 0);
    }
    xSemaphoreGive(runtime->console_lock);
}
