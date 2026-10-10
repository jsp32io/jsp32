#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "js_event.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* These functions must run in the EventLoop task. */
esp_err_t js_listener_add(js_runtime_t *runtime, uint16_t type,
                          uint16_t subtype, uint32_t source,
                          JSValueConst callback);
size_t js_listener_remove(js_runtime_t *runtime, uint16_t type,
                          uint16_t subtype, uint32_t source,
                          JSValueConst callback);
size_t js_listener_emit(js_runtime_t *runtime, uint16_t type,
                        uint16_t subtype, uint32_t source,
                        JSValueConst payload);

#ifdef __cplusplus
}
#endif
