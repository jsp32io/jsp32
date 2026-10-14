#include "js_abort.h"

#include <stdlib.h>
#include <string.h>

typedef struct abort_listener {
    JSValue callback;
    struct abort_listener *next;
} abort_listener_t;

typedef struct {
    bool aborted;
    JSValue reason;
    abort_listener_t *listeners;
    js_abort_subscription_t *requests;
} abort_signal_t;

struct js_abort_subscription {
    js_request_table_t *table;
    uint32_t id, generation;
    js_abort_subscription_t *next;
};

static JSClassID signal_class, controller_class;

js_cancel_t *js_cancel_new(void)
{
    js_cancel_t *token = malloc(sizeof(*token));
    if (token) {
        atomic_init(&token->references, 1);
        atomic_init(&token->cancelled, false);
    }
    return token;
}
void js_cancel_retain(js_cancel_t *token)
{
    if (token) atomic_fetch_add(&token->references, 1);
}
void js_cancel_release(void *opaque)
{
    js_cancel_t *token = opaque;
    if (token && atomic_fetch_sub(&token->references, 1) == 1) free(token);
}
void js_cancel_abort(uint32_t id, void *opaque)
{
    (void)id;
    js_cancel_t *token = opaque;
    if (token) atomic_store(&token->cancelled, true);
}
bool js_cancelled(const js_cancel_t *token)
{
    return token && atomic_load(&token->cancelled);
}

static void signal_finalizer(JSRuntime *rt, JSValue value)
{
    abort_signal_t *signal = JS_GetOpaque(value, signal_class);
    if (!signal) return;
    while (signal->listeners) {
        abort_listener_t *listener = signal->listeners;
        signal->listeners = listener->next;
        JS_FreeValueRT(rt, listener->callback);
        free(listener);
    }
    JS_FreeValueRT(rt, signal->reason);
    free(signal);
}
static void signal_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    abort_signal_t *signal = JS_GetOpaque(value, signal_class);
    if (!signal) return;
    JS_MarkValue(rt, signal->reason, mark);
    for (abort_listener_t *item = signal->listeners; item; item = item->next)
        JS_MarkValue(rt, item->callback, mark);
}
static void controller_finalizer(JSRuntime *rt, JSValue value)
{
    JSValue *signal = JS_GetOpaque(value, controller_class);
    if (signal) { JS_FreeValueRT(rt, *signal); free(signal); }
}
static void controller_mark(JSRuntime *rt, JSValueConst value, JS_MarkFunc *mark)
{
    JSValue *signal = JS_GetOpaque(value, controller_class);
    if (signal) JS_MarkValue(rt, *signal, mark);
}
static JSValue controller_new(JSContext *ctx, JSValueConst new_target,
                               int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    abort_signal_t *state = calloc(1, sizeof(*state));
    JSValue *stored = malloc(sizeof(*stored));
    if (!state || !stored) { free(state); free(stored); return JS_ThrowOutOfMemory(ctx); }
    state->reason = JS_UNDEFINED;
    JSValue signal = JS_NewObjectClass(ctx, signal_class);
    if (JS_IsException(signal)) { free(state); free(stored); return signal; }
    JS_SetOpaque(signal, state);
    JSValue controller = JS_NewObjectClass(ctx, controller_class);
    if (JS_IsException(controller)) { JS_FreeValue(ctx, signal); free(stored); return controller; }
    *stored = signal;
    JS_SetOpaque(controller, stored);
    return controller;
}
static JSValue signal_new(JSContext *ctx, JSValueConst new_target,
                           int argc, JSValueConst *argv)
{
    (void)new_target; (void)argc; (void)argv;
    return JS_ThrowTypeError(ctx, "AbortSignal cannot be constructed directly");
}
static JSValue controller_signal(JSContext *ctx, JSValueConst value)
{
    JSValue *signal = JS_GetOpaque2(ctx, value, controller_class);
    return signal ? JS_DupValue(ctx, *signal) : JS_EXCEPTION;
}
static JSValue signal_aborted(JSContext *ctx, JSValueConst value)
{
    abort_signal_t *signal = JS_GetOpaque2(ctx, value, signal_class);
    return signal ? JS_NewBool(ctx, signal->aborted) : JS_EXCEPTION;
}
static JSValue signal_reason(JSContext *ctx, JSValueConst value)
{
    abort_signal_t *signal = JS_GetOpaque2(ctx, value, signal_class);
    return signal ? JS_DupValue(ctx, signal->reason) : JS_EXCEPTION;
}
static JSValue signal_throw(JSContext *ctx, JSValueConst value, int argc, JSValueConst *argv)
{
    (void)argc; (void)argv;
    abort_signal_t *signal = JS_GetOpaque2(ctx, value, signal_class);
    if (!signal) return JS_EXCEPTION;
    return signal->aborted ? JS_Throw(ctx, JS_DupValue(ctx, signal->reason)) : JS_UNDEFINED;
}
static bool abort_event(JSContext *ctx, int argc, JSValueConst *argv)
{
    if (argc < 1 || !JS_IsString(argv[0])) {
        JS_ThrowTypeError(ctx, "event name must be abort"); return false;
    }
    const char *name = JS_ToCString(ctx, argv[0]);
    if (!name) return false;
    bool valid = strcmp(name, "abort") == 0;
    JS_FreeCString(ctx, name);
    if (!valid) JS_ThrowRangeError(ctx, "only abort events are supported");
    return valid;
}
static JSValue signal_add(JSContext *ctx, JSValueConst value, int argc, JSValueConst *argv)
{
    abort_signal_t *signal = JS_GetOpaque2(ctx, value, signal_class);
    if (!signal || !abort_event(ctx, argc, argv)) return JS_EXCEPTION;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) return JS_ThrowTypeError(ctx, "listener must be a function");
    if (signal->aborted) return JS_UNDEFINED;
    abort_listener_t **link = &signal->listeners;
    while (*link) {
        if (JS_StrictEq(ctx, (*link)->callback, argv[1]) == 1) return JS_UNDEFINED;
        link = &(*link)->next;
    }
    *link = calloc(1, sizeof(**link));
    if (!*link) return JS_ThrowOutOfMemory(ctx);
    (*link)->callback = JS_DupValue(ctx, argv[1]);
    return JS_UNDEFINED;
}
static JSValue signal_remove(JSContext *ctx, JSValueConst value, int argc, JSValueConst *argv)
{
    abort_signal_t *signal = JS_GetOpaque2(ctx, value, signal_class);
    if (!signal || !abort_event(ctx, argc, argv)) return JS_EXCEPTION;
    if (argc < 2) return JS_UNDEFINED;
    abort_listener_t **link = &signal->listeners;
    while (*link) {
        abort_listener_t *item = *link;
        if (JS_StrictEq(ctx, item->callback, argv[1]) == 1) {
            *link = item->next; JS_FreeValue(ctx, item->callback); free(item); break;
        }
        link = &item->next;
    }
    return JS_UNDEFINED;
}
static JSValue controller_abort(JSContext *ctx, JSValueConst value, int argc, JSValueConst *argv)
{
    JSValue *stored = JS_GetOpaque2(ctx, value, controller_class);
    if (!stored) return JS_EXCEPTION;
    abort_signal_t *signal = JS_GetOpaque(*stored, signal_class);
    if (signal->aborted) return JS_UNDEFINED;
    JSValue reason = argc > 0 ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
    if (JS_IsUndefined(reason)) {
        reason = JS_NewError(ctx);
        if (JS_IsException(reason)) return reason;
        if (JS_SetPropertyStr(ctx, reason, "name", JS_NewString(ctx, "AbortError")) < 0 ||
            JS_SetPropertyStr(ctx, reason, "code", JS_NewString(ctx, "ABORTED")) < 0 ||
            JS_SetPropertyStr(ctx, reason, "message", JS_NewString(ctx, "operation aborted")) < 0) {
            JS_FreeValue(ctx, reason); return JS_EXCEPTION;
        }
    }
    signal->aborted = true;
    signal->reason = reason;
    while (signal->requests) {
        js_abort_subscription_t *item = signal->requests;
        /* Cancellation detaches and frees this subscription before native hooks. */
        js_request_cancel(item->table, item->id, item->generation, reason);
    }
    abort_listener_t *listeners = signal->listeners;
    signal->listeners = NULL;
    JSValue event = JS_NewObject(ctx);
    if (!JS_IsException(event)) {
        JS_SetPropertyStr(ctx, event, "type", JS_NewString(ctx, "abort"));
        JS_SetPropertyStr(ctx, event, "target", JS_DupValue(ctx, *stored));
    }
    while (listeners) {
        abort_listener_t *item = listeners;
        listeners = item->next;
        if (!JS_IsException(event)) {
            JSValue result = JS_Call(ctx, item->callback, *stored, 1, &event);
            if (JS_IsException(result)) result = JS_GetException(ctx);
            JS_FreeValue(ctx, result);
        }
        JS_FreeValue(ctx, item->callback); free(item);
    }
    if (JS_IsException(event)) event = JS_GetException(ctx);
    JS_FreeValue(ctx, event);
    return JS_UNDEFINED;
}

JSValue js_abort_get_signal(JSContext *ctx, JSValueConst options)
{
    if (JS_IsUndefined(options)) return JS_UNDEFINED;
    JSValue signal = JS_GetPropertyStr(ctx, options, "signal");
    if (JS_IsException(signal) || JS_IsUndefined(signal)) return signal;
    if (!JS_GetOpaque(signal, signal_class)) {
        JS_FreeValue(ctx, signal); return JS_ThrowTypeError(ctx, "signal must be an AbortSignal");
    }
    return signal;
}
bool js_abort_aborted(JSContext *ctx, JSValueConst value)
{
    (void)ctx;
    abort_signal_t *signal = JS_GetOpaque(value, signal_class);
    return signal && signal->aborted;
}
JSValue js_abort_reason(JSContext *ctx, JSValueConst value)
{
    abort_signal_t *signal = JS_GetOpaque(value, signal_class);
    return signal ? JS_DupValue(ctx, signal->reason) : JS_UNDEFINED;
}
bool js_abort_attach(js_request_table_t *table, uint32_t id, uint32_t generation, JSValueConst value)
{
    if (JS_IsUndefined(value)) return true;
    abort_signal_t *signal = JS_GetOpaque2(table->ctx, value, signal_class);
    if (!signal) return false;
    if (signal->aborted) { js_request_cancel(table, id, generation, signal->reason); return true; }
    js_request_t *request = js_request_find(table, id, generation);
    if (!request || request->abort_subscription) {
        JS_ThrowInternalError(table->ctx, "abort subscription requires an unbound pending request");
        return false;
    }
    js_abort_subscription_t *item = malloc(sizeof(*item));
    if (!item) { JS_ThrowOutOfMemory(table->ctx); return false; }
    *item = (js_abort_subscription_t){.table = table, .id = id, .generation = generation,
                                    .next = signal->requests};
    signal->requests = item;
    request->abort_subscription = item;
    request->abort_signal = JS_DupValue(table->ctx, value);
    return true;
}
void js_abort_detach(js_request_t *request, JSContext *ctx)
{
    js_abort_subscription_t *item = request->abort_subscription;
    if (!item) return;
    abort_signal_t *signal = JS_GetOpaque(request->abort_signal, signal_class);
    js_abort_subscription_t **link = &signal->requests;
    while (*link && *link != item) link = &(*link)->next;
    if (*link) *link = item->next;
    free(item);
    request->abort_subscription = NULL;
    JS_FreeValue(ctx, request->abort_signal);
    request->abort_signal = JS_UNDEFINED;
}

int js_abort_init(JSContext *ctx)
{
    if (!signal_class) { JS_NewClassID(&signal_class); JS_NewClassID(&controller_class); }
    JSClassDef signal_def = {.class_name = "AbortSignal", .finalizer = signal_finalizer, .gc_mark = signal_mark};
    JSClassDef controller_def = {.class_name = "AbortController", .finalizer = controller_finalizer, .gc_mark = controller_mark};
    if (JS_NewClass(JS_GetRuntime(ctx), signal_class, &signal_def) < 0 ||
        JS_NewClass(JS_GetRuntime(ctx), controller_class, &controller_def) < 0) return -1;
    static const JSCFunctionListEntry signal_methods[] = {
        JS_CGETSET_DEF("aborted", signal_aborted, NULL),
        JS_CGETSET_DEF("reason", signal_reason, NULL),
        JS_CFUNC_DEF("throwIfAborted", 0, signal_throw),
        JS_CFUNC_DEF("addEventListener", 2, signal_add),
        JS_CFUNC_DEF("removeEventListener", 2, signal_remove),
    };
    static const JSCFunctionListEntry controller_methods[] = {
        JS_CGETSET_DEF("signal", controller_signal, NULL),
        JS_CFUNC_DEF("abort", 0, controller_abort),
    };
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue signal_proto = JS_NewObject(ctx), controller_proto = JS_NewObject(ctx);
    JSValue signal_ctor = JS_NewCFunction2(ctx, signal_new, "AbortSignal", 0, JS_CFUNC_constructor, 0);
    JSValue controller_ctor = JS_NewCFunction2(ctx, controller_new, "AbortController", 0, JS_CFUNC_constructor, 0);
    bool ok = !JS_IsException(global) && !JS_IsException(signal_proto) &&
        !JS_IsException(controller_proto) && !JS_IsException(signal_ctor) && !JS_IsException(controller_ctor);
    if (ok) ok = JS_SetPropertyFunctionList(ctx, signal_proto, signal_methods, 5) >= 0 &&
                  JS_SetPropertyFunctionList(ctx, controller_proto, controller_methods, 2) >= 0;
    if (ok) {
        JS_SetConstructor(ctx, signal_ctor, signal_proto);
        JS_SetConstructor(ctx, controller_ctor, controller_proto);
        JS_SetClassProto(ctx, signal_class, JS_DupValue(ctx, signal_proto));
        JS_SetClassProto(ctx, controller_class, JS_DupValue(ctx, controller_proto));
        ok = JS_SetPropertyStr(ctx, global, "AbortSignal", JS_DupValue(ctx, signal_ctor)) >= 0 &&
             JS_SetPropertyStr(ctx, global, "AbortController", JS_DupValue(ctx, controller_ctor)) >= 0;
    }
    JS_FreeValue(ctx, global); JS_FreeValue(ctx, signal_proto); JS_FreeValue(ctx, controller_proto);
    JS_FreeValue(ctx, signal_ctor); JS_FreeValue(ctx, controller_ctor);
    return ok ? 0 : -1;
}
