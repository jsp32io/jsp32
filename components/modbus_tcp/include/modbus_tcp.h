#pragma once

#include <stddef.h>
#include <stdint.h>
#include "app_registers.h"
#include "esp_err.h"

#define MODBUS_TCP_MAX_ADU 260

typedef struct modbus_tcp modbus_tcp_t;

/* Returns zero for an invalid ADU; otherwise returns the response length. */
size_t modbus_tcp_process(const uint8_t *request, size_t length,
                          uint8_t response[MODBUS_TCP_MAX_ADU],
                          app_registers_t *registers);
esp_err_t modbus_tcp_start(app_registers_t *registers, modbus_tcp_t **out_server);
