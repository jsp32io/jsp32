#include "app_registers.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

typedef struct {
    uint32_t id;
    app_register_event_fn callback;
    void *opaque;
} register_subscriber_t;

struct app_registers {
    SemaphoreHandle_t mutex;
    uint8_t *x;
    uint8_t *y;
    uint8_t *m;
    uint16_t *d;
    uint64_t revision;
    uint32_t next_subscriber_id;
    register_subscriber_t subscribers[CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS];
};

static size_t bit_storage_size(size_t count)
{
    return (count + 7) / 8;
}

size_t app_registers_area_count(app_register_area_t area)
{
    switch (area) {
    case APP_REGISTER_AREA_X: return CONFIG_APP_REGISTERS_X_COUNT;
    case APP_REGISTER_AREA_Y: return CONFIG_APP_REGISTERS_Y_COUNT;
    case APP_REGISTER_AREA_M: return CONFIG_APP_REGISTERS_M_COUNT;
    case APP_REGISTER_AREA_D: return CONFIG_APP_REGISTERS_D_COUNT;
    default: return 0;
    }
}

bool app_register_area_is_bit(app_register_area_t area)
{
    return area == APP_REGISTER_AREA_X || area == APP_REGISTER_AREA_Y ||
           area == APP_REGISTER_AREA_M;
}

const char *app_register_source_name(app_register_source_t source)
{
    switch (source) {
    case APP_REGISTER_SOURCE_JS: return "js";
    case APP_REGISTER_SOURCE_HTTP: return "http";
    case APP_REGISTER_SOURCE_DRIVER: return "driver";
    case APP_REGISTER_SOURCE_SYSTEM: return "system";
    case APP_REGISTER_SOURCE_MODBUS: return "modbus";
    default: return "unknown";
    }
}

static bool source_valid(app_register_source_t source)
{
    return source >= APP_REGISTER_SOURCE_JS &&
           source <= APP_REGISTER_SOURCE_MODBUS;
}

static bool address_valid(app_register_address_t address)
{
    size_t count = app_registers_area_count(address.area);
    return count > 0 && address.index < count;
}

static bool range_valid(app_register_address_t start, size_t count)
{
    size_t capacity = app_registers_area_count(start.area);
    return count > 0 && count <= CONFIG_APP_REGISTERS_MAX_TRANSACTION &&
           capacity > 0 && start.index < capacity &&
           count <= capacity - start.index;
}

static bool write_allowed(app_register_address_t address,
                          app_register_source_t source)
{
    return address.area != APP_REGISTER_AREA_X ||
           source == APP_REGISTER_SOURCE_JS ||
           source == APP_REGISTER_SOURCE_DRIVER ||
           source == APP_REGISTER_SOURCE_SYSTEM;
}

static uint8_t *bit_area(app_registers_t *registers,
                         app_register_area_t area)
{
    switch (area) {
    case APP_REGISTER_AREA_X: return registers->x;
    case APP_REGISTER_AREA_Y: return registers->y;
    case APP_REGISTER_AREA_M: return registers->m;
    default: return NULL;
    }
}

static uint16_t read_unlocked(app_registers_t *registers,
                              app_register_address_t address)
{
    if (address.area == APP_REGISTER_AREA_D) {
        return registers->d[address.index];
    }
    uint8_t *area = bit_area(registers, address.area);
    return (area[address.index / 8] >> (address.index % 8)) & 1U;
}

static void write_unlocked(app_registers_t *registers,
                           app_register_address_t address, uint16_t value)
{
    if (address.area == APP_REGISTER_AREA_D) {
        registers->d[address.index] = value;
        return;
    }
    uint8_t *area = bit_area(registers, address.area);
    uint8_t mask = (uint8_t)(1U << (address.index % 8));
    if (value) area[address.index / 8] |= mask;
    else area[address.index / 8] &= (uint8_t)~mask;
}

static bool take_lock(app_registers_t *registers)
{
    return xSemaphoreTake(registers->mutex, portMAX_DELAY) == pdTRUE;
}

static void give_lock(app_registers_t *registers)
{
    xSemaphoreGive(registers->mutex);
}

esp_err_t app_registers_init(app_registers_t **out_registers)
{
    if (!out_registers) return ESP_ERR_INVALID_ARG;
    *out_registers = NULL;
    app_registers_t *registers = calloc(1, sizeof(*registers));
    if (!registers) return ESP_ERR_NO_MEM;
    registers->mutex = xSemaphoreCreateMutex();
    registers->x = calloc(1, bit_storage_size(CONFIG_APP_REGISTERS_X_COUNT));
    registers->y = calloc(1, bit_storage_size(CONFIG_APP_REGISTERS_Y_COUNT));
    registers->m = calloc(1, bit_storage_size(CONFIG_APP_REGISTERS_M_COUNT));
    registers->d = calloc(CONFIG_APP_REGISTERS_D_COUNT,
                          sizeof(*registers->d));
    if (!registers->mutex || !registers->x || !registers->y ||
        !registers->m || !registers->d) {
        app_registers_deinit(registers);
        return ESP_ERR_NO_MEM;
    }
    *out_registers = registers;
    return ESP_OK;
}

void app_registers_deinit(app_registers_t *registers)
{
    if (!registers) return;
    if (registers->mutex) vSemaphoreDelete(registers->mutex);
    free(registers->x);
    free(registers->y);
    free(registers->m);
    free(registers->d);
    free(registers);
}

esp_err_t app_register_address_parse(const char *text,
                                     app_register_address_t *out_address)
{
    if (!text || !out_address || text[0] == '\0' || text[1] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    app_register_area_t area;
    switch (text[0]) {
    case 'X': area = APP_REGISTER_AREA_X; break;
    case 'Y': area = APP_REGISTER_AREA_Y; break;
    case 'M': area = APP_REGISTER_AREA_M; break;
    case 'D': area = APP_REGISTER_AREA_D; break;
    default: return ESP_ERR_INVALID_ARG;
    }
    if (text[1] == '+' || text[1] == '-' ||
        (text[1] == '0' && text[2] != '\0')) {
        return ESP_ERR_INVALID_ARG;
    }
    errno = 0;
    char *end = NULL;
    unsigned long index = strtoul(text + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0' || index > UINT16_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    app_register_address_t address = {
        .area = area,
        .index = (uint16_t)index,
    };
    if (!address_valid(address)) return ESP_ERR_INVALID_ARG;
    *out_address = address;
    return ESP_OK;
}

esp_err_t app_register_address_format(app_register_address_t address,
                                      char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0 || !address_valid(address)) {
        return ESP_ERR_INVALID_ARG;
    }
    static const char names[] = {'X', 'Y', 'M', 'D'};
    int written = snprintf(buffer, buffer_size, "%c%u", names[address.area],
                           address.index);
    return written >= 0 && (size_t)written < buffer_size
        ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t app_registers_read(app_registers_t *registers,
                             app_register_address_t address,
                             uint16_t *out_value,
                             uint64_t *out_revision)
{
    if (!registers || !out_value || !address_valid(address)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(registers)) return ESP_FAIL;
    *out_value = read_unlocked(registers, address);
    if (out_revision) *out_revision = registers->revision;
    give_lock(registers);
    return ESP_OK;
}

esp_err_t app_registers_read_many(app_registers_t *registers,
                                  const app_register_address_t *addresses,
                                  uint16_t *out_values, size_t count,
                                  uint64_t *out_revision)
{
    if (!registers || !addresses || !out_values || count == 0 ||
        count > CONFIG_APP_REGISTERS_MAX_TRANSACTION) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!address_valid(addresses[i])) return ESP_ERR_INVALID_ARG;
    }
    if (!take_lock(registers)) return ESP_FAIL;
    for (size_t i = 0; i < count; ++i) {
        out_values[i] = read_unlocked(registers, addresses[i]);
    }
    if (out_revision) *out_revision = registers->revision;
    give_lock(registers);
    return ESP_OK;
}

static void notify_subscribers(app_registers_t *registers,
                               const app_register_event_t *event)
{
    register_subscriber_t subscribers[CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS];
    size_t count = 0;
    if (!take_lock(registers)) return;
    for (size_t i = 0; i < CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS; ++i) {
        if (registers->subscribers[i].callback) {
            subscribers[count++] = registers->subscribers[i];
        }
    }
    give_lock(registers);
    for (size_t i = 0; i < count; ++i) {
        subscribers[i].callback(event, subscribers[i].opaque);
    }
}

esp_err_t app_registers_write_many(app_registers_t *registers,
                                   const app_register_write_t *writes,
                                   size_t count,
                                   app_register_source_t source,
                                   uint64_t *out_revision)
{
    if (!registers || !writes || count == 0 ||
        count > CONFIG_APP_REGISTERS_MAX_TRANSACTION ||
        !source_valid(source)) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!address_valid(writes[i].address)) return ESP_ERR_INVALID_ARG;
        if (!write_allowed(writes[i].address, source)) {
            return APP_REGISTERS_ERR_READ_ONLY;
        }
        if (app_register_area_is_bit(writes[i].address.area) &&
            writes[i].value > 1) {
            return ESP_ERR_INVALID_ARG;
        }
        for (size_t j = 0; j < i; ++j) {
            if (writes[i].address.area == writes[j].address.area &&
                writes[i].address.index == writes[j].address.index) {
                return APP_REGISTERS_ERR_DUPLICATE;
            }
        }
    }

    app_register_change_t *changes = calloc(count, sizeof(*changes));
    if (!changes) return ESP_ERR_NO_MEM;
    size_t changed = 0;
    uint64_t revision = 0;
    if (!take_lock(registers)) {
        free(changes);
        return ESP_FAIL;
    }
    for (size_t i = 0; i < count; ++i) {
        uint16_t old_value = read_unlocked(registers, writes[i].address);
        if (old_value == writes[i].value) continue;
        changes[changed++] = (app_register_change_t) {
            .address = writes[i].address,
            .old_value = old_value,
            .value = writes[i].value,
        };
    }
    if (changed > 0) {
        for (size_t i = 0; i < changed; ++i) {
            write_unlocked(registers, changes[i].address, changes[i].value);
        }
        ++registers->revision;
    }
    revision = registers->revision;
    give_lock(registers);
    if (out_revision) *out_revision = revision;

    if (changed > 0) {
        app_register_event_t event = {
            .revision = revision,
            .source = source,
            .changes = changes,
            .change_count = changed,
        };
        notify_subscribers(registers, &event);
    }
    free(changes);
    return ESP_OK;
}

esp_err_t app_registers_write(app_registers_t *registers,
                              app_register_address_t address,
                              uint16_t value,
                              app_register_source_t source,
                              uint64_t *out_revision)
{
    app_register_write_t write = {.address = address, .value = value};
    return app_registers_write_many(registers, &write, 1, source,
                                    out_revision);
}

esp_err_t app_registers_read_bits(app_registers_t *registers,
                                  app_register_address_t start,
                                  uint8_t *out_values, size_t count,
                                  uint64_t *out_revision)
{
    if (!registers || !out_values || !app_register_area_is_bit(start.area) ||
        !range_valid(start, count)) return ESP_ERR_INVALID_ARG;
    if (!take_lock(registers)) return ESP_FAIL;
    for (size_t i = 0; i < count; ++i) {
        app_register_address_t address = {
            .area = start.area,
            .index = (uint16_t)(start.index + i),
        };
        out_values[i] = (uint8_t)read_unlocked(registers, address);
    }
    if (out_revision) *out_revision = registers->revision;
    give_lock(registers);
    return ESP_OK;
}

esp_err_t app_registers_read_words(app_registers_t *registers,
                                   app_register_address_t start,
                                   uint16_t *out_values, size_t count,
                                   uint64_t *out_revision)
{
    if (!registers || !out_values || start.area != APP_REGISTER_AREA_D ||
        !range_valid(start, count)) return ESP_ERR_INVALID_ARG;
    if (!take_lock(registers)) return ESP_FAIL;
    memcpy(out_values, &registers->d[start.index],
           count * sizeof(*out_values));
    if (out_revision) *out_revision = registers->revision;
    give_lock(registers);
    return ESP_OK;
}

esp_err_t app_registers_write_bits(app_registers_t *registers,
                                   app_register_address_t start,
                                   const uint8_t *values, size_t count,
                                   app_register_source_t source,
                                   uint64_t *out_revision)
{
    if (!registers || !values || !app_register_area_is_bit(start.area) ||
        !range_valid(start, count)) return ESP_ERR_INVALID_ARG;
    app_register_write_t *writes = calloc(count, sizeof(*writes));
    if (!writes) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        if (values[i] > 1) {
            free(writes);
            return ESP_ERR_INVALID_ARG;
        }
        writes[i].address = (app_register_address_t) {
            .area = start.area,
            .index = (uint16_t)(start.index + i),
        };
        writes[i].value = values[i];
    }
    esp_err_t err = app_registers_write_many(registers, writes, count,
                                              source, out_revision);
    free(writes);
    return err;
}

esp_err_t app_registers_write_words(app_registers_t *registers,
                                    app_register_address_t start,
                                    const uint16_t *values, size_t count,
                                    app_register_source_t source,
                                    uint64_t *out_revision)
{
    if (!registers || !values || start.area != APP_REGISTER_AREA_D ||
        !range_valid(start, count)) return ESP_ERR_INVALID_ARG;
    app_register_write_t *writes = calloc(count, sizeof(*writes));
    if (!writes) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < count; ++i) {
        writes[i].address = (app_register_address_t) {
            .area = APP_REGISTER_AREA_D,
            .index = (uint16_t)(start.index + i),
        };
        writes[i].value = values[i];
    }
    esp_err_t err = app_registers_write_many(registers, writes, count,
                                              source, out_revision);
    free(writes);
    return err;
}

uint64_t app_registers_revision(app_registers_t *registers)
{
    if (!registers || !take_lock(registers)) return 0;
    uint64_t revision = registers->revision;
    give_lock(registers);
    return revision;
}

esp_err_t app_registers_subscribe(app_registers_t *registers,
                                  app_register_event_fn callback,
                                  void *opaque, uint32_t *out_id)
{
    if (!registers || !callback || !out_id) return ESP_ERR_INVALID_ARG;
    if (!take_lock(registers)) return ESP_FAIL;
    size_t slot = CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS;
    for (size_t i = 0; i < CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS; ++i) {
        if (!registers->subscribers[i].callback) {
            slot = i;
            break;
        }
    }
    if (slot == CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS) {
        give_lock(registers);
        return ESP_ERR_NO_MEM;
    }
    uint32_t id = ++registers->next_subscriber_id;
    if (id == 0) id = ++registers->next_subscriber_id;
    registers->subscribers[slot] = (register_subscriber_t) {
        .id = id,
        .callback = callback,
        .opaque = opaque,
    };
    give_lock(registers);
    *out_id = id;
    return ESP_OK;
}

bool app_registers_unsubscribe(app_registers_t *registers, uint32_t id)
{
    if (!registers || id == 0 || !take_lock(registers)) return false;
    bool removed = false;
    for (size_t i = 0; i < CONFIG_APP_REGISTERS_MAX_SUBSCRIBERS; ++i) {
        if (registers->subscribers[i].id == id) {
            memset(&registers->subscribers[i], 0,
                   sizeof(registers->subscribers[i]));
            removed = true;
            break;
        }
    }
    give_lock(registers);
    return removed;
}
