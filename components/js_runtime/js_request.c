#include "js_request.h"

#include <stdlib.h>

void js_request_table_init(js_request_table_t *table, JSContext *ctx)
{
    *table = (js_request_table_t){.ctx = ctx};
}

static void settle(JSContext *ctx, JSValueConst function, JSValueConst value)
{
    JSValue result = JS_Call(ctx, function, JS_UNDEFINED, 1, &value);
    if (JS_IsException(result)) result = JS_GetException(ctx);
    JS_FreeValue(ctx, result);
}

JSValue js_request_promise(JSContext *ctx, bool rejected, JSValueConst value)
{
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(ctx, functions);
    if (JS_IsException(promise)) return promise;
    settle(ctx, functions[rejected ? 1 : 0], value);
    JS_FreeValue(ctx, functions[0]);
    JS_FreeValue(ctx, functions[1]);
    return promise;
}

static bool id_in_use(const js_request_table_t *table, uint32_t id)
{
    for (js_request_t *item = table->head; item; item = item->next) {
        if (item->id == id) return true;
    }
    return false;
}

JSValue js_request_create(js_request_table_t *table, uint32_t generation,
                          void *opaque, js_request_dispose_fn dispose,
                          js_request_cancel_fn cancel, uint32_t *out_id)
{
    *out_id = 0;
    if (table->closing) return JS_ThrowInternalError(table->ctx, "request table closed");
    js_request_t *request = calloc(1, sizeof(*request));
    if (!request) return JS_ThrowOutOfMemory(table->ctx);
    JSValue functions[2];
    JSValue promise = JS_NewPromiseCapability(table->ctx, functions);
    if (JS_IsException(promise)) { free(request); return promise; }
    do {
        if (++table->next_id == 0) ++table->next_id;
    } while (id_in_use(table, table->next_id));
    *request = (js_request_t){.id = table->next_id, .generation = generation,
        .opaque = opaque, .resolve = functions[0], .reject = functions[1],
        .cancel = cancel, .dispose = dispose, .next = table->head};
    table->head = request;
    ++table->count;
    *out_id = request->id;
    return promise;
}

js_request_t *js_request_find(js_request_table_t *table, uint32_t id,
                              uint32_t generation)
{
    for (js_request_t *item = table->head; item; item = item->next) {
        if (item->id == id && item->generation == generation) return item;
    }
    return NULL;
}

static bool complete(js_request_table_t *table, uint32_t id, uint32_t generation,
                      JSValueConst value, bool rejected, bool cancelled)
{
    js_request_t **link = &table->head;
    while (*link && ((*link)->id != id || (*link)->generation != generation))
        link = &(*link)->next;
    if (!*link) return false;
    js_request_t *request = *link;
    *link = request->next;
    --table->count;
    if (cancelled && request->cancel) request->cancel(id, request->opaque);
    settle(table->ctx, rejected ? request->reject : request->resolve, value);
    JS_FreeValue(table->ctx, request->resolve);
    JS_FreeValue(table->ctx, request->reject);
    if (request->dispose) request->dispose(request->opaque);
    free(request);
    return true;
}

bool js_request_resolve(js_request_table_t *table, uint32_t id,
                         uint32_t generation, JSValueConst value)
{
    return complete(table, id, generation, value, false, false);
}

bool js_request_reject(js_request_table_t *table, uint32_t id,
                        uint32_t generation, JSValueConst error)
{
    return complete(table, id, generation, error, true, false);
}

bool js_request_cancel(js_request_table_t *table, uint32_t id,
                        uint32_t generation, JSValueConst error)
{
    return complete(table, id, generation, error, true, true);
}

void js_request_table_close(js_request_table_t *table, JSValueConst error)
{
    table->closing = true;
    while (table->head) {
        js_request_t *request = table->head;
        js_request_cancel(table, request->id, request->generation, error);
    }
}

bool js_request_set_deadline(js_request_table_t *table, uint32_t id,
                              uint32_t generation, int64_t deadline_us)
{
    js_request_t *request = js_request_find(table, id, generation);
    if (!request || deadline_us < 0) return false;
    request->deadline_us = deadline_us;
    return true;
}

int64_t js_request_next_deadline(const js_request_table_t *table)
{
    int64_t deadline = 0;
    for (js_request_t *item = table->head; item; item = item->next) {
        if (item->deadline_us && (!deadline || item->deadline_us < deadline))
            deadline = item->deadline_us;
    }
    return deadline;
}

size_t js_request_expire(js_request_table_t *table, int64_t now_us,
                          JSValueConst error)
{
    size_t count = 0;
    /* Bound each pass even if cancellation hooks create more expired requests. */
    size_t budget = table->count;
    while (count < budget) {
        js_request_t *expired = NULL;
        for (js_request_t *item = table->head; item; item = item->next) {
            if (item->deadline_us && item->deadline_us <= now_us) { expired = item; break; }
        }
        if (!expired) break;
        js_request_cancel(table, expired->id, expired->generation, error);
        ++count;
    }
    return count;
}
