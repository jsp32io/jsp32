#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_REGISTERS_ERR_READ_ONLY 0x10201
#define APP_REGISTERS_ERR_DUPLICATE 0x10202

typedef struct app_registers app_registers_t;

typedef enum {
    APP_REGISTER_AREA_X,
    APP_REGISTER_AREA_Y,
    APP_REGISTER_AREA_M,
    APP_REGISTER_AREA_D,
} app_register_area_t;

typedef enum {
    APP_REGISTER_SOURCE_JS,
    APP_REGISTER_SOURCE_HTTP,
    APP_REGISTER_SOURCE_DRIVER,
    APP_REGISTER_SOURCE_SYSTEM,
    APP_REGISTER_SOURCE_MODBUS,
} app_register_source_t;

typedef struct {
    app_register_area_t area;
    uint16_t index;
} app_register_address_t;

typedef struct {
    app_register_address_t address;
    uint16_t value;
} app_register_write_t;

typedef struct {
    app_register_address_t address;
    uint16_t old_value;
    uint16_t value;
} app_register_change_t;

typedef struct {
    uint64_t revision;
    app_register_source_t source;
    const app_register_change_t *changes;
    size_t change_count;
} app_register_event_t;

typedef void (*app_register_event_fn)(const app_register_event_t *event,
                                      void *opaque);

esp_err_t app_registers_init(app_registers_t **out_registers);
void app_registers_deinit(app_registers_t *registers);

esp_err_t app_register_address_parse(const char *text,
                                     app_register_address_t *out_address);
esp_err_t app_register_address_format(app_register_address_t address,
                                      char *buffer, size_t buffer_size);
size_t app_registers_area_count(app_register_area_t area);
bool app_register_area_is_bit(app_register_area_t area);
const char *app_register_source_name(app_register_source_t source);

esp_err_t app_registers_read(app_registers_t *registers,
                             app_register_address_t address,
                             uint16_t *out_value,
                             uint64_t *out_revision);
esp_err_t app_registers_write(app_registers_t *registers,
                              app_register_address_t address,
                              uint16_t value,
                              app_register_source_t source,
                              uint64_t *out_revision);

esp_err_t app_registers_read_bits(app_registers_t *registers,
                                  app_register_address_t start,
                                  uint8_t *out_values, size_t count,
                                  uint64_t *out_revision);
esp_err_t app_registers_write_bits(app_registers_t *registers,
                                   app_register_address_t start,
                                   const uint8_t *values, size_t count,
                                   app_register_source_t source,
                                   uint64_t *out_revision);
esp_err_t app_registers_read_words(app_registers_t *registers,
                                   app_register_address_t start,
                                   uint16_t *out_values, size_t count,
                                   uint64_t *out_revision);
esp_err_t app_registers_write_words(app_registers_t *registers,
                                    app_register_address_t start,
                                    const uint16_t *values, size_t count,
                                    app_register_source_t source,
                                    uint64_t *out_revision);

esp_err_t app_registers_read_many(app_registers_t *registers,
                                  const app_register_address_t *addresses,
                                  uint16_t *out_values, size_t count,
                                  uint64_t *out_revision);
esp_err_t app_registers_write_many(app_registers_t *registers,
                                   const app_register_write_t *writes,
                                   size_t count,
                                   app_register_source_t source,
                                   uint64_t *out_revision);

uint64_t app_registers_revision(app_registers_t *registers);
esp_err_t app_registers_subscribe(app_registers_t *registers,
                                  app_register_event_fn callback,
                                  void *opaque, uint32_t *out_id);
bool app_registers_unsubscribe(app_registers_t *registers, uint32_t id);

#ifdef __cplusplus
}
#endif
