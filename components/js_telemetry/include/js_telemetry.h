#pragma once
#include "app_mqtt.h"

/* Register before js_runtime_start(). Service must outlive all JS runtimes. */
esp_err_t js_telemetry_register(app_mqtt_t *service);
