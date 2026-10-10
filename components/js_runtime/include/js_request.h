#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*js_request_cancel_fn)(uint32_t id, void *opaque);
typedef void (*js_request_dispose_fn)(void *opaque);
typedef struct js_request js_request_t;
struct js_request {
    uint32_t id;
    uint32_t generation;
    void *opaque;
    js_request_t *next;
    /* Owned by the table; modules must not call or free these values. */
    JSValue resolve;
    JSValue reject;
    js_request_cancel_fn cancel;
    js_request_dispose_fn dispose;
    int64_t deadline_us;
};

typedef struct js_request_table {
    JSContext *ctx;
    js_request_t *head;
    size_t count;
    uint32_t next_id;
    bool closing;
    struct js_request_table *next_table;
} js_request_table_t;

/* All operations run on the EventLoop task. Initialize an empty table once. */
void js_request_table_init(js_request_table_t *table, JSContext *ctx);
/* On success retains opaque and calls dispose (if supplied) on completion.
 * Failure leaves opaque with the caller. ID zero is invalid. */
JSValue js_request_create(js_request_table_t *table, uint32_t generation,
                          void *opaque, js_request_dispose_fn dispose,
                          js_request_cancel_fn cancel, uint32_t *out_id);
js_request_t *js_request_find(js_request_table_t *table, uint32_t id,
                              uint32_t generation);
/* Values are borrowed. Completion detaches before settling and releases opaque. */
bool js_request_resolve(js_request_table_t *table, uint32_t id,
                         uint32_t generation, JSValueConst value);
bool js_request_reject(js_request_table_t *table, uint32_t id,
                        uint32_t generation, JSValueConst error);
bool js_request_cancel(js_request_table_t *table, uint32_t id,
                        uint32_t generation, JSValueConst error);
/* Permanently closes the table, invokes cancel hooks, and rejects all requests. */
void js_request_table_close(js_request_table_t *table, JSValueConst error);
/* Optional deadlines use the caller's monotonic microsecond clock. Zero disables. */
bool js_request_set_deadline(js_request_table_t *table, uint32_t id,
                              uint32_t generation, int64_t deadline_us);
int64_t js_request_next_deadline(const js_request_table_t *table);
size_t js_request_expire(js_request_table_t *table, int64_t now_us,
                          JSValueConst error);
/* Immediate settled Promise, without registering a request. Value is borrowed. */
JSValue js_request_promise(JSContext *ctx, bool rejected, JSValueConst value);

#ifdef __cplusplus
}
#endif
