#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t allowed_port_mask;
    uint64_t allowed_pin_mask;
} js_uart_config_t;

#define JS_UART_CONFIG_DEFAULT()                                      \
    ((js_uart_config_t) {                                             \
        .allowed_port_mask = UINT32_MAX,                              \
        .allowed_pin_mask = UINT64_MAX,                               \
    })

/* Must be called before js_runtime_start(). The configuration is copied. */
esp_err_t js_uart_register(const js_uart_config_t *config);

#ifdef __cplusplus
}
#endif
