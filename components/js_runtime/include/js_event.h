#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct js_runtime js_runtime_t;

typedef enum {
    JS_EVENT_STOP = 1,
    JS_EVENT_USER = 0x100,
    JS_EVENT_GPIO = 0x200,
    JS_EVENT_UART = 0x300,
    JS_EVENT_HTTP = 0x400,
    JS_EVENT_BLE = 0x500,
    JS_EVENT_I2C = 0x600,
    JS_EVENT_REGISTERS = 0x700,
    JS_EVENT_NET = 0x800,
} js_event_type_t;

typedef void (*js_event_payload_free_fn)(void *payload);

typedef struct {
    uint16_t type;
    uint16_t subtype;
    uint32_t source;
    uint32_t request_id;
    int64_t timestamp_us;
    void *payload;
    size_t payload_size;
    js_event_payload_free_fn free_payload;
} js_event_t;

typedef void (*js_event_handler_fn)(js_runtime_t *runtime,
                                    const js_event_t *event,
                                    void *opaque);

/* Takes ownership of event->payload whether the send succeeds or fails. */
esp_err_t js_event_post(js_runtime_t *runtime, const js_event_t *event,
                        TickType_t wait_ticks);

/* ISR events must not contain a payload or a payload release callback. */
esp_err_t js_event_post_from_isr(js_runtime_t *runtime,
                                 const js_event_t *event,
                                 BaseType_t *higher_priority_task_woken);

/* Handler registration must run in the EventLoop task. */
esp_err_t js_event_register_handler(js_runtime_t *runtime, uint16_t type,
                                    js_event_handler_fn handler, void *opaque);
esp_err_t js_event_unregister_handler(js_runtime_t *runtime, uint16_t type);

void js_event_release(js_event_t *event);

#ifdef __cplusplus
}
#endif
