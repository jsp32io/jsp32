#pragma once

#include "app_registers.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Must be called before js_runtime_start(). The store must outlive the runtime. */
esp_err_t js_registers_register(app_registers_t *registers);

#ifdef __cplusplus
}
#endif
