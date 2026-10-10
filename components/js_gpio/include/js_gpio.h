#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t allowed_pin_mask;
} js_gpio_config_t;

#define JS_GPIO_CONFIG_DEFAULT() \
    ((js_gpio_config_t) {.allowed_pin_mask = UINT64_MAX})

/* Must be called before js_runtime_start(). The configuration is copied. */
esp_err_t js_gpio_register(const js_gpio_config_t *config);

#ifdef __cplusplus
}
#endif
