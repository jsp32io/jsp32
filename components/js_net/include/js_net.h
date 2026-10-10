#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Installs net and SocketError. Call before js_runtime_start(). */
esp_err_t js_net_register(void);

#ifdef __cplusplus
}
#endif
