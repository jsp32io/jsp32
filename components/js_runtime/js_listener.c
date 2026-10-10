#include "js_listener.h"

#include <stdlib.h>

#include "js_runtime.h"
#include "js_runtime_internal.h"

struct js_listener {
    uint16_t type;
    uint16_t subtype;
    uint32_t source;
    JSValue callback;
    js_listener_t *next;
};

esp_err_t js_listener_init(js_runtime_t *runtime)
{
    runtime->listeners = NULL;
    return ESP_OK;
}

static bool listener_matches(const js_listener_t *listener, uint16_t type,
                             uint16_t subtype, uint32_t source)
{
    return listener->type == type && listener->subtype == subtype &&
           listener->source == source;
}

esp_err_t js_listener_add(js_runtime_t *runtime, uint16_t type,
                          uint16_t subtype, uint32_t source,
                          JSValueConst callback)
{
    if (!js_runtime_is_owner(runtime) ||
        !JS_IsFunction(runtime->ctx, callback)) {
        return ESP_ERR_INVALID_ARG;
    }

    js_listener_t *listener = calloc(1, sizeof(*listener));
    if (!listener) {
        return ESP_ERR_NO_MEM;
    }
    listener->type = type;
    listener->subtype = subtype;
    listener->source = source;
    listener->callback = JS_DupValue(runtime->ctx, callback);
    listener->next = runtime->listeners;
    runtime->listeners = listener;
    return ESP_OK;
}

size_t js_listener_remove(js_runtime_t *runtime, uint16_t type,
                          uint16_t subtype, uint32_t source,
                          JSValueConst callback)
{
    if (!js_runtime_is_owner(runtime)) {
        return 0;
    }

    size_t removed = 0;
    js_listener_t **link = &runtime->listeners;
    while (*link) {
        js_listener_t *listener = *link;
        bool same_callback = JS_IsUndefined(callback) ||
            JS_StrictEq(runtime->ctx, listener->callback, callback) == 1;
        if (listener_matches(listener, type, subtype, source) && same_callback) {
            *link = listener->next;
            JS_FreeValue(runtime->ctx, listener->callback);
            free(listener);
            ++removed;
            continue;
        }
        link = &listener->next;
    }
    return removed;
}

size_t js_listener_emit(js_runtime_t *runtime, uint16_t type,
                        uint16_t subtype, uint32_t source,
                        JSValueConst payload)
{
    if (!js_runtime_is_owner(runtime)) {
        return 0;
    }

    size_t count = 0;
    for (js_listener_t *item = runtime->listeners; item; item = item->next) {
        if (listener_matches(item, type, subtype, source)) {
            ++count;
        }
    }
    if (count == 0) {
        return 0;
    }

    JSValue *callbacks = calloc(count, sizeof(*callbacks));
    if (!callbacks) {
        return 0;
    }
    size_t index = 0;
    for (js_listener_t *item = runtime->listeners; item; item = item->next) {
        if (listener_matches(item, type, subtype, source)) {
            callbacks[index++] = JS_DupValue(runtime->ctx, item->callback);
        }
    }

    for (size_t i = 0; i < count; ++i) {
        JSValueConst argv[] = {payload};
        js_runtime_call(runtime, callbacks[i], JS_UNDEFINED, 1, argv);
        JS_FreeValue(runtime->ctx, callbacks[i]);
    }
    free(callbacks);
    return count;
}

void js_listener_deinit(js_runtime_t *runtime)
{
    js_listener_t *listener = runtime->listeners;
    runtime->listeners = NULL;
    while (listener) {
        js_listener_t *next = listener->next;
        JS_FreeValue(runtime->ctx, listener->callback);
        free(listener);
        listener = next;
    }
}
