#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t allowed_pin_mask;
} js_pcnt_config_t;

#define JS_PCNT_CONFIG_DEFAULT() \
    ((js_pcnt_config_t) {.allowed_pin_mask = UINT64_MAX})

/* Call before js_runtime_start(); the configuration is copied. */
esp_err_t js_pcnt_register(const js_pcnt_config_t *config);

#ifdef __cplusplus
}
#endif
