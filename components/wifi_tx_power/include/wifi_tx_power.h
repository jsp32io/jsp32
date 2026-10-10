#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Returns the saved limit, or the firmware default when no limit is saved. */
esp_err_t wifi_tx_power_load(int8_t *power);
esp_err_t wifi_tx_power_save(int8_t power);
