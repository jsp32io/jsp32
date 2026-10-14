#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "js_request.h"
#include "js_abort.h"
#include <string.h>

typedef struct {
    js_request_table_t *table;
    uint32_t id;
    uint32_t generation;
    int *cancelled;
    int *disposed;
} payload_t;

static void dispose(void *opaque)
{
    payload_t *payload = opaque;
    ++*payload->disposed;
    free(payload);
}

static void cancel(uint32_t id, void *opaque)
{
    payload_t *payload = opaque;
    assert(id == payload->id);
    /* Detached before calling native hooks; a reentrant completion is ignored. */
    assert(!js_request_find(payload->table, id, payload->generation));
    assert(!js_request_resolve(payload->table, id, payload->generation, JS_UNDEFINED));
    ++*payload->cancelled;
}

static JSValue request(js_request_table_t *table, uint32_t generation,
                        int *cancelled, int *disposed, uint32_t *id)
{
    payload_t *payload = calloc(1, sizeof(*payload));
    assert(payload);
    *payload = (payload_t){.table = table, .generation = generation,
        .cancelled = cancelled, .disposed = disposed};
    JSValue promise = js_request_create(table, generation, payload, dispose, cancel, id);
    assert(!JS_IsException(promise));
    payload->id = *id;
    return promise;
}

static void assert_result(JSContext *ctx, JSValueConst promise, JSPromiseStateEnum state, int expected)
{
    assert(JS_PromiseState(ctx, promise) == state);
    JSValue result = JS_PromiseResult(ctx, promise);
    int value;
    assert(!JS_ToInt32(ctx, &value, result));
    assert(value == expected);
    JS_FreeValue(ctx, result);
}

static void lifecycle(JSContext *ctx)
{
    js_request_table_t table;
    js_request_table_init(&table, ctx);
    uint32_t id;
    int cancelled = 0, disposed = 0;
    JSValue promise = request(&table, 7, &cancelled, &disposed, &id);
    assert(table.count == 1 && id != 0);
    assert(!js_request_resolve(&table, id, 8, JS_NewInt32(ctx, 99)));
    assert(JS_PromiseState(ctx, promise) == JS_PROMISE_PENDING);
    assert(js_request_resolve(&table, id, 7, JS_NewInt32(ctx, 42)));
    assert_result(ctx, promise, JS_PROMISE_FULFILLED, 42);
    assert(disposed == 1 && cancelled == 0 && table.count == 0);
    assert(!js_request_reject(&table, id, 7, JS_UNDEFINED));
    assert(!js_request_cancel(&table, id, 7, JS_UNDEFINED));
    JS_FreeValue(ctx, promise);

    promise = request(&table, 8, &cancelled, &disposed, &id);
    assert(js_request_reject(&table, id, 8, JS_NewInt32(ctx, 12)));
    assert_result(ctx, promise, JS_PROMISE_REJECTED, 12);
    assert(disposed == 2 && cancelled == 0);
    JS_FreeValue(ctx, promise);

    promise = request(&table, 9, &cancelled, &disposed, &id);
    assert(js_request_cancel(&table, id, 9, JS_NewInt32(ctx, 13)));
    assert_result(ctx, promise, JS_PROMISE_REJECTED, 13);
    assert(disposed == 3 && cancelled == 1);
    assert(!js_request_resolve(&table, id, 9, JS_UNDEFINED));
    JS_FreeValue(ctx, promise);
}

static void deadlines_and_wrap(JSContext *ctx)
{
    js_request_table_t table;
    js_request_table_init(&table, ctx);
    uint32_t ids[3];
    int cancelled = 0, disposed = 0;
    JSValue promises[3];
    promises[0] = request(&table, 1, &cancelled, &disposed, &ids[0]);
    table.next_id = UINT32_MAX;
    promises[1] = request(&table, 1, &cancelled, &disposed, &ids[1]);
    promises[2] = request(&table, 2, &cancelled, &disposed, &ids[2]);
    assert(ids[1] != 0 && ids[1] != ids[0]);
    assert(ids[2] != ids[1]);
    assert(!js_request_set_deadline(&table, ids[0], 2, 10));
    assert(!js_request_set_deadline(&table, ids[0], 1, -1));
    assert(js_request_set_deadline(&table, ids[0], 1, 100));
    assert(js_request_set_deadline(&table, ids[1], 1, 200));
    assert(js_request_next_deadline(&table) == 100);
    assert(js_request_expire(&table, 99, JS_NewInt32(ctx, 408)) == 0);
    assert(js_request_expire(&table, 100, JS_NewInt32(ctx, 408)) == 1);
    assert_result(ctx, promises[0], JS_PROMISE_REJECTED, 408);
    assert(js_request_next_deadline(&table) == 200);
    assert(JS_PromiseState(ctx, promises[1]) == JS_PROMISE_PENDING);
    assert(js_request_set_deadline(&table, ids[1], 1, 0));
    assert(js_request_next_deadline(&table) == 0);
    assert(js_request_expire(&table, INT64_MAX, JS_UNDEFINED) == 0);
    js_request_table_close(&table, JS_NewInt32(ctx, 410));
    assert_result(ctx, promises[1], JS_PROMISE_REJECTED, 410);
    assert_result(ctx, promises[2], JS_PROMISE_REJECTED, 410);
    assert(table.count == 0 && !table.head && cancelled == 3 && disposed == 3);
    js_request_table_close(&table, JS_UNDEFINED);
    JSValue failed = js_request_create(&table, 1, NULL, NULL, NULL, &ids[0]);
    assert(JS_IsException(failed) && ids[0] == 0);
    JS_FreeValue(ctx, JS_GetException(ctx));
    for (int i = 0; i < 3; ++i) JS_FreeValue(ctx, promises[i]);
}

static void failed_creation(JSContext *ctx)
{
    js_request_table_t table;
    js_request_table_init(&table, ctx);
    int cancelled = 0, disposed = 0;
    payload_t *payload = calloc(1, sizeof(*payload));
    assert(payload);
    payload->disposed = &disposed;
    payload->cancelled = &cancelled;
    uint32_t id = 999;
    JS_SetMemoryLimit(JS_GetRuntime(ctx), 1);
    JSValue promise = js_request_create(&table, 1, payload, dispose, cancel, &id);
    assert(JS_IsException(promise));
    assert(id == 0 && table.count == 0 && table.head == NULL);
    assert(disposed == 0 && cancelled == 0);
    JS_SetMemoryLimit(JS_GetRuntime(ctx), (size_t)-1);
    JS_FreeValue(ctx, JS_GetException(ctx));
    dispose(payload); // Failure leaves opaque owned by the caller.
    assert(disposed == 1);
}

static void immediate_and_borrowed(JSContext *ctx)
{
    JSValue value = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, value, "answer", JS_NewInt32(ctx, 42));
    JSValue promise = js_request_promise(ctx, false, value);
    JS_FreeValue(ctx, value);
    assert(JS_PromiseState(ctx, promise) == JS_PROMISE_FULFILLED);
    JSValue result = JS_PromiseResult(ctx, promise);
    JSValue answer = JS_GetPropertyStr(ctx, result, "answer");
    int number;
    assert(!JS_ToInt32(ctx, &number, answer) && number == 42);
    JS_FreeValue(ctx, answer);
    JS_FreeValue(ctx, result);
    JS_FreeValue(ctx, promise);
    promise = js_request_promise(ctx, true, JS_NewInt32(ctx, 400));
    assert_result(ctx, promise, JS_PROMISE_REJECTED, 400);
    JS_FreeValue(ctx, promise);
}

static JSValue evaluate(JSContext *ctx, const char *source)
{
    JSValue value = JS_Eval(ctx, source, strlen(source), "abort-test", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(value)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "%s\n", message);
        JS_FreeCString(ctx, message); JS_FreeValue(ctx, error);
        abort();
    }
    return value;
}

static void abort_signals(JSContext *ctx)
{
    assert(js_abort_init(ctx) == 0);
    JSValue script = evaluate(ctx,
        "globalThis.controller = new AbortController();"
        "if (!(controller.signal instanceof AbortSignal) || controller.signal.aborted) throw Error('initial');"
        "globalThis.calls = 0; const listener = () => calls++;"
        "controller.signal.addEventListener('abort', listener);"
        "controller.signal.addEventListener('abort', listener);"
        "controller.signal.addEventListener('abort', () => { throw Error('listener'); });");
    JS_FreeValue(ctx, script);
    JSValue options = evaluate(ctx, "({signal: controller.signal})");
    JSValue signal = js_abort_get_signal(ctx, options);
    assert(!JS_IsException(signal));
    js_request_table_t table;
    js_request_table_init(&table, ctx);
    int cancelled = 0, disposed = 0;
    uint32_t id;
    JSValue promise = request(&table, 1, &cancelled, &disposed, &id);
    assert(js_abort_attach(&table, id, 1, signal));
    script = evaluate(ctx, "controller.abort(); controller.abort('later');");
    JS_FreeValue(ctx, script);
    assert(table.count == 0 && cancelled == 1 && disposed == 1);
    assert(JS_PromiseState(ctx, promise) == JS_PROMISE_REJECTED);
    JSValue reason = js_abort_reason(ctx, signal);
    JSValue result = JS_PromiseResult(ctx, promise);
    assert(JS_StrictEq(ctx, result, reason) == 1);
    assert(!js_request_resolve(&table, id, 1, JS_UNDEFINED));
    script = evaluate(ctx,
        "if (calls !== 1 || controller.signal.reason.name !== 'AbortError' ||"
        "controller.signal.reason.code !== 'ABORTED') throw Error('abort event');"
        "let thrown=false; try {controller.signal.throwIfAborted();} catch(e) {thrown=e===controller.signal.reason;}"
        "if (!thrown) throw Error('throwIfAborted');");
    JS_FreeValue(ctx, script);
    JS_FreeValue(ctx, result); JS_FreeValue(ctx, reason); JS_FreeValue(ctx, promise);
    promise = request(&table, 1, &cancelled, &disposed, &id);
    assert(js_abort_attach(&table, id, 1, signal));
    assert(table.count == 0 && cancelled == 2 && disposed == 2);
    JS_FreeValue(ctx, promise); JS_FreeValue(ctx, signal); JS_FreeValue(ctx, options);

    options = evaluate(ctx, "globalThis.completed = new AbortController(); ({signal: completed.signal})");
    signal = js_abort_get_signal(ctx, options);
    promise = request(&table, 2, &cancelled, &disposed, &id);
    assert(js_abort_attach(&table, id, 2, signal));
    assert(js_request_resolve(&table, id, 2, JS_TRUE));
    script = evaluate(ctx, "completed.abort('ignored');");
    JS_FreeValue(ctx, script);
    assert(cancelled == 2 && disposed == 3 && JS_PromiseState(ctx, promise) == JS_PROMISE_FULFILLED);
    JS_FreeValue(ctx, promise); JS_FreeValue(ctx, signal); JS_FreeValue(ctx, options);

    options = evaluate(ctx, "({signal: {aborted:false}})");
    signal = js_abort_get_signal(ctx, options);
    assert(JS_IsException(signal));
    JS_FreeValue(ctx, JS_GetException(ctx)); JS_FreeValue(ctx, options);
    js_cancel_t *token = js_cancel_new();
    assert(token && !js_cancelled(token));
    js_cancel_retain(token); js_cancel_abort(1, token); js_cancel_release(token);
    assert(js_cancelled(token)); js_cancel_release(token);
}

int main(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    assert(ctx);
    abort_signals(ctx);
    lifecycle(ctx);
    deadlines_and_wrap(ctx);
    immediate_and_borrowed(ctx);
    failed_creation(ctx);
    JS_RunGC(runtime);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    puts("js_request lifecycle, deadlines, cancellation and cleanup passed");
    return 0;
}
