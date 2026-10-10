#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Advertise the JSP32 HTTP API through DNS-SD on active network interfaces. */
esp_err_t device_discovery_start(uint16_t http_port);

#ifdef __cplusplus
}
#endif
