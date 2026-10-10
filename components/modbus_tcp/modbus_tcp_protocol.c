#include "modbus_tcp.h"

#include <stdbool.h>
#include <string.h>
#include "sdkconfig.h"

enum { ILLEGAL_FUNCTION = 1, ILLEGAL_ADDRESS = 2, ILLEGAL_VALUE = 3,
       DEVICE_FAILURE = 4 };
#define MAX_BITS_PER_REQUEST \
    (CONFIG_APP_REGISTERS_MAX_TRANSACTION < 256 ? CONFIG_APP_REGISTERS_MAX_TRANSACTION : 256)

static uint16_t be16(const uint8_t *p) { return ((uint16_t)p[0] << 8) | p[1]; }
static void put16(uint8_t *p, uint16_t value) { p[0] = value >> 8; p[1] = value; }

static bool locate_bits(uint8_t function, uint16_t start, uint16_t count,
                        app_register_address_t *address)
{
    size_t x = app_registers_area_count(APP_REGISTER_AREA_X);
    size_t y = app_registers_area_count(APP_REGISTER_AREA_Y);
    size_t m = app_registers_area_count(APP_REGISTER_AREA_M);
    if (function == 2) {
        if ((size_t)start + count > x) return false;
        *address = (app_register_address_t){APP_REGISTER_AREA_X, start};
    } else if (start < y) {
        if ((size_t)start + count > y) return false;
        *address = (app_register_address_t){APP_REGISTER_AREA_Y, start};
    } else {
        if ((size_t)start - y + count > m) return false;
        *address = (app_register_address_t){APP_REGISTER_AREA_M,
            (uint16_t)(start - y)};
    }
    return true;
}

static bool locate_words(uint8_t function, uint16_t start, uint16_t count,
                         app_register_address_t *address)
{
    size_t d = app_registers_area_count(APP_REGISTER_AREA_D);
    size_t split = d / 2;
    size_t base = function == 4 ? 0 : split;
    size_t capacity = function == 4 ? split : d - split;
    if ((size_t)start + count > capacity) return false;
    *address = (app_register_address_t){APP_REGISTER_AREA_D,
        (uint16_t)(base + start)};
    return true;
}

size_t modbus_tcp_process(const uint8_t *request, size_t length,
                          uint8_t response[MODBUS_TCP_MAX_ADU],
                          app_registers_t *registers)
{
    if (!request || !response || !registers || length < 8 ||
        length > MODBUS_TCP_MAX_ADU || be16(request + 2) != 0 ||
        be16(request + 4) != length - 6 || be16(request + 4) < 2) return 0;
    const uint8_t *pdu = request + 7;
    size_t pdu_len = length - 7;
    uint8_t function = pdu[0];
    uint8_t *out = response + 7;
    size_t out_len = 0;
    uint8_t error = 0;
    app_register_address_t address;
    uint16_t start, count;

    if (function >= 1 && function <= 4) {
        if (pdu_len != 5) { error = ILLEGAL_VALUE; goto done; }
        start = be16(pdu + 1); count = be16(pdu + 3);
        bool bits = function <= 2;
        if (count == 0 || count > MAX_BITS_PER_REQUEST ||
            count > (bits ? 2000 : 125)) { error = ILLEGAL_VALUE; goto done; }
        if (!(bits ? locate_bits(function, start, count, &address) :
                      locate_words(function, start, count, &address))) {
            error = ILLEGAL_ADDRESS; goto done;
        }
        out[0] = function; out[1] = bits ? (count + 7) / 8 : count * 2;
        out_len = (size_t)out[1] + 2;
        if (bits) {
            uint8_t values[MAX_BITS_PER_REQUEST];
            if (app_registers_read_bits(registers, address, values, count,
                                        NULL) != ESP_OK) {
                error = DEVICE_FAILURE; goto done;
            }
            memset(out + 2, 0, out[1]);
            for (size_t i = 0; i < count; ++i)
                out[2 + i / 8] |= (values[i] & 1U) << (i % 8);
        } else {
            uint16_t values[125];
            if (app_registers_read_words(registers, address, values, count,
                                         NULL) != ESP_OK) {
                error = DEVICE_FAILURE; goto done;
            }
            for (size_t i = 0; i < count; ++i) put16(out + 2 + i * 2, values[i]);
        }
    } else if (function == 5 || function == 6) {
        if (pdu_len != 5) { error = ILLEGAL_VALUE; goto done; }
        start = be16(pdu + 1);
        uint16_t value = be16(pdu + 3);
        if (function == 5 && value != 0 && value != 0xff00) {
            error = ILLEGAL_VALUE; goto done;
        }
        if (!(function == 5 ? locate_bits(5, start, 1, &address) :
                             locate_words(6, start, 1, &address))) {
            error = ILLEGAL_ADDRESS; goto done;
        }
        esp_err_t result = function == 5 ?
            app_registers_write_bits(registers, address,
                                     (uint8_t[]){value == 0xff00}, 1,
                                     APP_REGISTER_SOURCE_MODBUS, NULL) :
            app_registers_write_words(registers, address, &value, 1,
                                      APP_REGISTER_SOURCE_MODBUS, NULL);
        if (result != ESP_OK) { error = DEVICE_FAILURE; goto done; }
        memcpy(out, pdu, 5); out_len = 5;
    } else if (function == 15 || function == 16) {
        if (pdu_len < 6) { error = ILLEGAL_VALUE; goto done; }
        start = be16(pdu + 1); count = be16(pdu + 3);
        bool bits = function == 15;
        size_t bytes = bits ? ((size_t)count + 7) / 8 : (size_t)count * 2;
        if (count == 0 || count > (bits ? MAX_BITS_PER_REQUEST : CONFIG_APP_REGISTERS_MAX_TRANSACTION) ||
            count > (bits ? 1968 : 123) || bytes != pdu[5] ||
            pdu_len != bytes + 6) { error = ILLEGAL_VALUE; goto done; }
        if (!(bits ? locate_bits(15, start, count, &address) :
                      locate_words(16, start, count, &address))) {
            error = ILLEGAL_ADDRESS; goto done;
        }
        esp_err_t result;
        if (bits) {
            uint8_t values[MAX_BITS_PER_REQUEST];
            for (size_t i = 0; i < count; ++i)
                values[i] = (pdu[6 + i / 8] >> (i % 8)) & 1U;
            result = app_registers_write_bits(registers, address, values, count,
                                              APP_REGISTER_SOURCE_MODBUS, NULL);
        } else {
            uint16_t values[123];
            for (size_t i = 0; i < count; ++i) values[i] = be16(pdu + 6 + i * 2);
            result = app_registers_write_words(registers, address, values, count,
                                               APP_REGISTER_SOURCE_MODBUS, NULL);
        }
        if (result != ESP_OK) { error = DEVICE_FAILURE; goto done; }
        memcpy(out, pdu, 5); out_len = 5;
    } else {
        error = ILLEGAL_FUNCTION;
    }

done:
    if (error) { out[0] = function | 0x80; out[1] = error; out_len = 2; }
    memcpy(response, request, 2);
    response[2] = 0; response[3] = 0;
    put16(response + 4, (uint16_t)(out_len + 1));
    response[6] = request[6];
    return out_len + 7;
}
