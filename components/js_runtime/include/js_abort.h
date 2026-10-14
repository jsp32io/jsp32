#pragma once

#include <stdatomic.h>
#include "js_request.h"

typedef struct js_abort_subscription js_abort_subscription_t;
typedef struct {
    atomic_uint references;
    atomic_bool cancelled;
} js_cancel_t;

/* Native tokens never contain JSValues. Each owner releases its reference. */
js_cancel_t *js_cancel_new(void);
void js_cancel_retain(js_cancel_t *token);
void js_cancel_release(void *token);
void js_cancel_abort(uint32_t id, void *token);
bool js_cancelled(const js_cancel_t *token);

int js_abort_init(JSContext *ctx);
/* Returns an owned, branded signal or undefined; malformed signals throw. */
JSValue js_abort_get_signal(JSContext *ctx, JSValueConst options);
bool js_abort_aborted(JSContext *ctx, JSValueConst signal);
JSValue js_abort_reason(JSContext *ctx, JSValueConst signal);
/* Attaches to an existing request; already-aborted signals cancel immediately. */
bool js_abort_attach(js_request_table_t *table, uint32_t id, uint32_t generation,
                     JSValueConst signal);
void js_abort_detach(js_request_t *request, JSContext *ctx);
