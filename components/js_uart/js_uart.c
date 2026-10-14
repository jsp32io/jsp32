#include "js_uart.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/uart_ll.h"
#include "js_event.h"
#include "js_runtime.h"
#include "js_request.h"
#include "quickjs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_JS_UART_ENABLED

#define UART_BAUD_MIN 300
#define UART_BAUD_MAX 5000000
#define UART_BUFFER_MIN 256
#define UART_BUFFER_MAX 16384
#define UART_READ_TIMEOUT_MAX_MS 60000
#define UART_WORKER_POLL_MS 10

typedef enum {
    UART_OPERATION_READ,
    UART_OPERATION_READ_FRAME,
    UART_OPERATION_WRITE,
} uart_operation_type_t;

typedef enum {
    UART_FRAME_NONE,
    UART_FRAME_IDLE,
    UART_FRAME_FIXED,
    UART_FRAME_DELIMITER,
    UART_FRAME_LENGTH_FIELD,
} uart_frame_type_t;

typedef struct {
    uart_frame_type_t type;
    size_t max_bytes;
    uint8_t rx_timeout_chars;
    size_t fixed_length;
    uint8_t delimiter;
    size_t length_offset;
    size_t length_width;
    size_t length_adjustment;
    bool length_big_endian;
    uint32_t gap_us;
} uart_frame_config_t;

typedef struct {
    uint32_t id;
    uint32_t generation;
    uart_operation_type_t type;
    uint8_t *data;
    size_t size;
    uint32_t timeout_ms;
    int64_t deadline_us;
} uart_command_t;

typedef struct {
    uint32_t id;
    uint32_t generation;
    uart_operation_type_t type;
    uint8_t *data;
    size_t size;
    esp_err_t status;
} uart_completion_t;

typedef js_request_t uart_request_t;

typedef struct js_uart_module js_uart_module_t;

typedef struct {
    js_uart_module_t *module;
    uart_port_t id;
    QueueHandle_t commands;
    QueueHandle_t completions;
    QueueHandle_t uart_events;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    js_request_table_t requests;
    uint64_t manually_reserved_pin_mask;
    uint32_t generation;
    int tx_pin;
    int rx_pin;
    uart_frame_config_t frame;
    size_t event_bytes_remaining;
    portMUX_TYPE lock;
    bool open;
    bool closing;
} uart_port_state_t;

struct js_uart_module {
    js_runtime_t *runtime;
    JSContext *ctx;
    uint32_t allowed_port_mask;
    uint64_t allowed_pin_mask;
    uart_port_state_t ports[UART_NUM_MAX];
    bool initialized;
};

typedef struct {
    int tx;
    int rx;
    int baud_rate;
    int rx_buffer_size;
    int tx_buffer_size;
    uart_word_length_t data_bits;
    uart_stop_bits_t stop_bits;
    uart_parity_t parity;
    int direction_pin;
    uart_frame_config_t frame;
} uart_open_options_t;

static js_uart_module_t s_uart;
static bool s_registered;

static bool port_is_console(uart_port_t port)
{
#if defined(CONFIG_ESP_CONSOLE_UART_NUM) && CONFIG_ESP_CONSOLE_UART_NUM >= 0
    return port == CONFIG_ESP_CONSOLE_UART_NUM;
#else
    (void)port;
    return false;
#endif
}

static bool port_allowed(const js_uart_module_t *module, uart_port_t port)
{
    return port >= 0 && port < 32 &&
           (module->allowed_port_mask & (UINT32_C(1) << port)) != 0;
}

static bool pin_allowed(const js_uart_module_t *module, int pin)
{
    return pin >= 0 && pin < 64 &&
           (module->allowed_pin_mask & (UINT64_C(1) << pin)) != 0;
}

static bool state_is_closing(uart_port_state_t *state)
{
    portENTER_CRITICAL(&state->lock);
    bool closing = state->closing;
    portEXIT_CRITICAL(&state->lock);
    return closing;
}

static void clear_exception(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
}

static JSValue throw_esp_error(JSContext *ctx, const char *operation,
                               esp_err_t err)
{
    return JS_ThrowInternalError(ctx, "%s failed: %s", operation,
                                 esp_err_to_name(err));
}

static bool get_integer(JSContext *ctx, JSValueConst value, const char *name,
                        int32_t min, int32_t max, int32_t *out)
{
    if (!JS_IsNumber(value)) {
        JS_ThrowTypeError(ctx, "%s must be an integer", name);
        return false;
    }
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value)) return false;
    if (!isfinite(number) || floor(number) != number ||
        number < min || number > max) {
        JS_ThrowRangeError(ctx, "%s is out of range", name);
        return false;
    }
    *out = (int32_t)number;
    return true;
}

static bool get_port_id(JSContext *ctx, int argc, JSValueConst *argv,
                        uart_port_t *out)
{
    int32_t id = 0;
    if (argc < 1 ||
        !get_integer(ctx, argv[0], "UART id", 0, UART_NUM_MAX - 1, &id)) {
        return false;
    }
    *out = (uart_port_t)id;
    return true;
}

static bool known_open_field(const char *name)
{
    return strcmp(name, "tx") == 0 || strcmp(name, "rx") == 0 ||
           strcmp(name, "baudRate") == 0 ||
           strcmp(name, "dataBits") == 0 ||
           strcmp(name, "stopBits") == 0 ||
           strcmp(name, "parity") == 0 ||
           strcmp(name, "rxBufferSize") == 0 ||
           strcmp(name, "txBufferSize") == 0 ||
           strcmp(name, "framing") == 0 ||
           strcmp(name, "halfDuplex") == 0;
}

static bool validate_open_fields(JSContext *ctx, JSValueConst options)
{
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, options,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return false;
    }
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) {
            valid = false;
            break;
        }
        if (!known_open_field(name)) {
            JS_ThrowTypeError(ctx, "unknown UART option: %s", name);
            valid = false;
        }
        JS_FreeCString(ctx, name);
        if (!valid) break;
    }
    JS_FreePropertyEnum(ctx, properties, count);
    return valid;
}

static bool get_optional_integer(JSContext *ctx, JSValueConst options,
                                 const char *name, int32_t min, int32_t max,
                                 int *value, bool *present)
{
    JSValue property = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsException(property)) return false;
    if (JS_IsUndefined(property)) {
        *present = false;
        JS_FreeValue(ctx, property);
        return true;
    }
    int32_t parsed = 0;
    bool ok = get_integer(ctx, property, name, min, max, &parsed);
    JS_FreeValue(ctx, property);
    if (!ok) return false;
    *present = true;
    *value = parsed;
    return true;
}

static bool parse_read_options(JSContext *ctx, JSValueConst options,
                               int32_t *timeout_ms)
{
    *timeout_ms = 0;
    if (JS_IsUndefined(options)) return true;
    if (!JS_IsObject(options) || JS_IsArray(ctx, options) ||
        JS_IsFunction(ctx, options)) {
        JS_ThrowTypeError(ctx, "read options must be an object");
        return false;
    }
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, options,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return false;
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) { valid = false; break; }
        if (strcmp(name, "timeoutMs") != 0) {
            JS_ThrowTypeError(ctx, "unknown read option: %s", name);
            valid = false;
        }
        JS_FreeCString(ctx, name);
        if (!valid) break;
    }
    JS_FreePropertyEnum(ctx, properties, count);
    if (!valid) return false;
    JSValue value = JS_GetPropertyStr(ctx, options, "timeoutMs");
    if (JS_IsException(value)) return false;
    valid = JS_IsUndefined(value) || get_integer(ctx, value, "timeoutMs", 0,
                                                UART_READ_TIMEOUT_MAX_MS, timeout_ms);
    JS_FreeValue(ctx, value);
    return valid;
}

static bool parse_framing(JSContext *ctx, JSValueConst options,
                          uart_frame_config_t *frame)
{
    JSValue value = JS_GetPropertyStr(ctx, options, "framing");
    if (JS_IsException(value)) return false;
    if (JS_IsUndefined(value)) {
        JS_FreeValue(ctx, value);
        return true;
    }
    if (!JS_IsObject(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "framing must be an object");
        return false;
    }
    JSValue type_value = JS_GetPropertyStr(ctx, value, "type");
    if (JS_IsException(type_value)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    const char *type = JS_IsString(type_value) ? JS_ToCString(ctx, type_value) : NULL;
    if (type) {
        if (strcmp(type, "idle") == 0) frame->type = UART_FRAME_IDLE;
        else if (strcmp(type, "fixed") == 0) frame->type = UART_FRAME_FIXED;
        else if (strcmp(type, "delimiter") == 0) frame->type = UART_FRAME_DELIMITER;
        else if (strcmp(type, "lengthField") == 0) frame->type = UART_FRAME_LENGTH_FIELD;
        JS_FreeCString(ctx, type);
    }
    JS_FreeValue(ctx, type_value);
    if (frame->type == UART_FRAME_NONE) {
        JS_FreeValue(ctx, value);
        JS_ThrowRangeError(ctx, "framing.type must be idle, fixed, delimiter, or lengthField");
        return false;
    }
    int max_bytes = CONFIG_JS_UART_MAX_TRANSFER_SIZE < 256
        ? CONFIG_JS_UART_MAX_TRANSFER_SIZE : 256;
    int number = 0;
    bool present = false;
    if (!get_optional_integer(ctx, value, "maxBytes", 1,
                              CONFIG_JS_UART_MAX_TRANSFER_SIZE, &max_bytes, &present)) {
        JS_FreeValue(ctx, value);
        return false;
    }
    frame->max_bytes = (size_t)max_bytes;
    bool ok = true;
    switch (frame->type) {
        case UART_FRAME_IDLE:
            frame->rx_timeout_chars = 4;
            {
                JSValue gap = JS_GetPropertyStr(ctx, value, "gapChars");
                if (JS_IsException(gap)) ok = false;
                else if (!JS_IsUndefined(gap)) {
                    double chars = 0;
                    if (!JS_IsNumber(gap)) {
                        JS_ThrowTypeError(ctx, "framing.gapChars must be a number");
                        ok = false;
                    } else if (JS_ToFloat64(ctx, &chars, gap)) ok = false;
                    else if (!isfinite(chars) || chars < 1 || chars > 126) {
                        JS_ThrowRangeError(ctx, "framing.gapChars is out of range");
                        ok = false;
                    } else frame->rx_timeout_chars = (uint8_t)ceil(chars);
                }
                JS_FreeValue(ctx, gap);
            }
            break;
        case UART_FRAME_FIXED:
            number = 0;
            ok = get_optional_integer(ctx, value, "length", 1, max_bytes, &number, &present);
            if (ok && !present) {
                JS_ThrowTypeError(ctx, "framing.length is required");
                ok = false;
            }
            if (ok) frame->fixed_length = (size_t)number;
            break;
        case UART_FRAME_DELIMITER:
            number = 0;
            ok = get_optional_integer(ctx, value, "byte", 0, 255, &number, &present);
            if (ok && !present) {
                JS_ThrowTypeError(ctx, "framing.byte is required");
                ok = false;
            }
            if (ok) frame->delimiter = (uint8_t)number;
            break;
        case UART_FRAME_LENGTH_FIELD:
            number = 0;
            ok = get_optional_integer(ctx, value, "offset", 0, max_bytes - 1, &number, &present);
            if (ok) frame->length_offset = (size_t)number;
            if (ok) {
                number = 1;
                ok = get_optional_integer(ctx, value, "width", 1, 2, &number, &present);
                if (ok) frame->length_width = (size_t)number;
            }
            if (ok) {
                number = 0;
                ok = get_optional_integer(ctx, value, "adjustment", 0, max_bytes, &number, &present);
                if (ok) frame->length_adjustment = (size_t)number;
            }
            if (ok) {
                JSValue endian = JS_GetPropertyStr(ctx, value, "endian");
                if (JS_IsException(endian)) ok = false;
                else if (!JS_IsUndefined(endian)) {
                    const char *name = JS_IsString(endian) ? JS_ToCString(ctx, endian) : NULL;
                    if (!name || (strcmp(name, "big") != 0 && strcmp(name, "little") != 0)) {
                        JS_ThrowRangeError(ctx, "framing.endian must be big or little");
                        ok = false;
                    } else frame->length_big_endian = strcmp(name, "big") == 0;
                    if (name) JS_FreeCString(ctx, name);
                }
                JS_FreeValue(ctx, endian);
            }
            if (ok && frame->length_offset + frame->length_width > frame->max_bytes) {
                JS_ThrowRangeError(ctx, "length field exceeds maxBytes");
                ok = false;
            }
            break;
        default: break;
    }
    JS_FreeValue(ctx, value);
    return ok;
}

static bool parse_half_duplex(JSContext *ctx, JSValueConst options,
                              int *direction_pin)
{
    JSValue value = JS_GetPropertyStr(ctx, options, "halfDuplex");
    if (JS_IsException(value)) return false;
    if (JS_IsUndefined(value)) {
        JS_FreeValue(ctx, value);
        return true;
    }
    if (!JS_IsObject(value) || JS_IsNull(value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "halfDuplex must be an object");
        return false;
    }
    bool present = false;
    bool ok = get_optional_integer(ctx, value, "directionPin", 0,
                                   GPIO_NUM_MAX - 1, direction_pin, &present);
    JS_FreeValue(ctx, value);
    if (ok && !present) {
        JS_ThrowTypeError(ctx, "halfDuplex.directionPin is required");
        return false;
    }
    return ok;
}

static bool parse_stop_bits(JSContext *ctx, JSValueConst options,
                            uart_stop_bits_t *out)
{
    JSValue property = JS_GetPropertyStr(ctx, options, "stopBits");
    if (JS_IsException(property)) return false;
    if (JS_IsUndefined(property)) {
        *out = UART_STOP_BITS_1;
        JS_FreeValue(ctx, property);
        return true;
    }
    if (!JS_IsNumber(property)) {
        JS_FreeValue(ctx, property);
        JS_ThrowTypeError(ctx, "stopBits must be 1, 1.5, or 2");
        return false;
    }
    double value = 0;
    bool ok = JS_ToFloat64(ctx, &value, property) == 0;
    JS_FreeValue(ctx, property);
    if (!ok) return false;
    if (value == 1) *out = UART_STOP_BITS_1;
    else if (value == 1.5) *out = UART_STOP_BITS_1_5;
    else if (value == 2) *out = UART_STOP_BITS_2;
    else {
        JS_ThrowRangeError(ctx, "stopBits must be 1, 1.5, or 2");
        return false;
    }
    return true;
}

static bool parse_parity(JSContext *ctx, JSValueConst options,
                         uart_parity_t *out)
{
    JSValue property = JS_GetPropertyStr(ctx, options, "parity");
    if (JS_IsException(property)) return false;
    if (JS_IsUndefined(property)) {
        *out = UART_PARITY_DISABLE;
        JS_FreeValue(ctx, property);
        return true;
    }
    if (!JS_IsString(property)) {
        JS_FreeValue(ctx, property);
        JS_ThrowTypeError(ctx, "parity must be a string");
        return false;
    }
    const char *value = JS_ToCString(ctx, property);
    JS_FreeValue(ctx, property);
    if (!value) return false;
    bool valid = true;
    if (strcmp(value, "none") == 0) *out = UART_PARITY_DISABLE;
    else if (strcmp(value, "even") == 0) *out = UART_PARITY_EVEN;
    else if (strcmp(value, "odd") == 0) *out = UART_PARITY_ODD;
    else valid = false;
    JS_FreeCString(ctx, value);
    if (!valid) {
        JS_ThrowRangeError(ctx, "parity must be none, even, or odd");
    }
    return valid;
}

static bool parse_open_options(JSContext *ctx, JSValueConst value,
                               uart_open_options_t *out)
{
    if (!JS_IsObject(value) || JS_IsNull(value)) {
        JS_ThrowTypeError(ctx, "UART options must be an object");
        return false;
    }
    if (!validate_open_fields(ctx, value)) return false;

    *out = (uart_open_options_t) {
        .tx = UART_PIN_NO_CHANGE,
        .rx = UART_PIN_NO_CHANGE,
        .baud_rate = 115200,
        .rx_buffer_size = CONFIG_JS_UART_DEFAULT_BUFFER_SIZE,
        .tx_buffer_size = CONFIG_JS_UART_DEFAULT_BUFFER_SIZE,
        .data_bits = UART_DATA_8_BITS,
        .stop_bits = UART_STOP_BITS_1,
        .parity = UART_PARITY_DISABLE,
        .direction_pin = UART_PIN_NO_CHANGE,
    };

    bool tx_present = false;
    bool rx_present = false;
    bool present = false;
    if (!get_optional_integer(ctx, value, "tx", 0, GPIO_NUM_MAX - 1,
                              &out->tx, &tx_present) ||
        !get_optional_integer(ctx, value, "rx", 0, GPIO_NUM_MAX - 1,
                              &out->rx, &rx_present) ||
        !get_optional_integer(ctx, value, "baudRate", UART_BAUD_MIN,
                              UART_BAUD_MAX, &out->baud_rate, &present) ||
        !get_optional_integer(ctx, value, "rxBufferSize", UART_BUFFER_MIN,
                              UART_BUFFER_MAX, &out->rx_buffer_size,
                              &present) ||
        !get_optional_integer(ctx, value, "txBufferSize", UART_BUFFER_MIN,
                              UART_BUFFER_MAX, &out->tx_buffer_size,
                              &present)) {
        return false;
    }
    int data_bits = 8;
    if (!get_optional_integer(ctx, value, "dataBits", 5, 8,
                              &data_bits, &present)) {
        return false;
    }
    switch (data_bits) {
        case 5: out->data_bits = UART_DATA_5_BITS; break;
        case 6: out->data_bits = UART_DATA_6_BITS; break;
        case 7: out->data_bits = UART_DATA_7_BITS; break;
        case 8: out->data_bits = UART_DATA_8_BITS; break;
        default: return false;
    }
    if (!parse_stop_bits(ctx, value, &out->stop_bits) ||
        !parse_parity(ctx, value, &out->parity) ||
        !parse_framing(ctx, value, &out->frame) ||
        !parse_half_duplex(ctx, value, &out->direction_pin)) {
        return false;
    }
    if (!tx_present && !rx_present) {
        JS_ThrowTypeError(ctx, "at least one of tx and rx is required");
        return false;
    }
    if (tx_present && !GPIO_IS_VALID_OUTPUT_GPIO(out->tx)) {
        JS_ThrowRangeError(ctx, "invalid UART TX pin");
        return false;
    }
    if (rx_present && !GPIO_IS_VALID_GPIO(out->rx)) {
        JS_ThrowRangeError(ctx, "invalid UART RX pin");
        return false;
    }
    if (tx_present && rx_present && out->tx == out->rx) {
        JS_ThrowRangeError(ctx, "tx and rx must use different pins");
        return false;
    }
    if (out->frame.type != UART_FRAME_NONE && !rx_present) {
        JS_ThrowTypeError(ctx, "framing requires RX");
        return false;
    }
    if (out->direction_pin != UART_PIN_NO_CHANGE &&
        (!tx_present || !rx_present ||
         !GPIO_IS_VALID_OUTPUT_GPIO(out->direction_pin) ||
         out->direction_pin == out->tx || out->direction_pin == out->rx)) {
        JS_ThrowRangeError(ctx, "halfDuplex requires distinct TX, RX, and output directionPin");
        return false;
    }
    if (out->frame.type == UART_FRAME_IDLE) {
        double stop_bits = out->stop_bits == UART_STOP_BITS_2 ? 2.0
            : out->stop_bits == UART_STOP_BITS_1_5 ? 1.5 : 1.0;
        double bits = data_bits + (out->parity == UART_PARITY_DISABLE ? 0 : 1) +
                      stop_bits + 1;
        out->frame.gap_us = (uint32_t)ceil(
            (double)out->frame.rx_timeout_chars * bits * 1000000.0 /
            out->baud_rate);
    }
    return true;
}

static void post_completion_wake(uart_port_state_t *state)
{
    js_event_t event = {
        .type = JS_EVENT_UART,
        .source = (uint32_t)state->id,
        .request_id = state->generation,
    };
    js_event_post(state->module->runtime, &event, 0);
}

static TickType_t poll_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(UART_WORKER_POLL_MS);
    return ticks > 0 ? ticks : 1;
}

static uart_completion_t execute_read(uart_port_state_t *state,
                                      uart_command_t *command)
{
    uart_completion_t result = {
        .id = command->id,
        .generation = command->generation,
        .type = command->type,
        .data = command->data,
        .status = ESP_OK,
    };
    command->data = NULL;
    int64_t deadline = command->deadline_us;
    for (;;) {
        TickType_t wait = poll_ticks();
        if (command->timeout_ms > 0) {
            int64_t remaining = deadline - esp_timer_get_time();
            if (remaining <= 0) { result.status = ESP_ERR_TIMEOUT; break; }
            uint32_t wait_ms = (uint32_t)((remaining + 999) / 1000);
            if (wait_ms > UART_WORKER_POLL_MS) wait_ms = UART_WORKER_POLL_MS;
            wait = pdMS_TO_TICKS(wait_ms);
            if (wait == 0) wait = 1;
        }
        int count = uart_read_bytes(state->id, result.data, command->size,
                                    wait);
        if (count < 0) {
            result.status = ESP_FAIL;
            break;
        }
        if (count > 0) {
            result.size = (size_t)count;
            break;
        }
        if (state_is_closing(state)) break;
    }
    if (state_is_closing(state)) result.status = ESP_ERR_INVALID_STATE;
    return result;
}

static size_t framed_expected_length(const uart_frame_config_t *frame,
                                      const uint8_t *data, size_t size)
{
    if (size < frame->length_offset + frame->length_width) return 0;
    size_t offset = frame->length_offset;
    size_t payload = frame->length_width == 1 ? data[offset]
        : frame->length_big_endian
            ? ((size_t)data[offset] << 8) | data[offset + 1]
            : ((size_t)data[offset + 1] << 8) | data[offset];
    return payload + frame->length_adjustment;
}

static bool frame_complete(const uart_frame_config_t *frame,
                           const uint8_t *data, size_t size)
{
    switch (frame->type) {
        case UART_FRAME_FIXED: return size == frame->fixed_length;
        case UART_FRAME_DELIMITER: return data[size - 1] == frame->delimiter;
        case UART_FRAME_LENGTH_FIELD: {
            size_t expected = framed_expected_length(frame, data, size);
            return expected != 0 && size == expected;
        }
        default: return false;
    }
}

static uart_completion_t execute_read_frame(uart_port_state_t *state,
                                            uart_command_t *command)
{
    uart_completion_t result = {
        .id = command->id,
        .generation = command->generation,
        .type = command->type,
        .data = command->data,
        .status = ESP_OK,
    };
    command->data = NULL;
    int64_t deadline = command->deadline_us;
    int64_t last_data_event_us = 0;
    while (!state_is_closing(state)) {
        if (deadline && esp_timer_get_time() >= deadline) {
            result.status = ESP_ERR_TIMEOUT;
            break;
        }
        if (state->event_bytes_remaining > 0) {
            uint8_t byte = 0;
            int n = uart_read_bytes(state->id, &byte, 1, 0);
            if (n != 1) {
                result.status = ESP_FAIL;
                break;
            }
            --state->event_bytes_remaining;
            if (result.size >= state->frame.max_bytes) {
                result.status = ESP_ERR_INVALID_SIZE;
                break;
            }
            result.data[result.size++] = byte;
            if (state->frame.type == UART_FRAME_LENGTH_FIELD &&
                result.size >= state->frame.length_offset + state->frame.length_width) {
                size_t expected = framed_expected_length(&state->frame,
                                                          result.data, result.size);
                if (expected < result.size || expected > state->frame.max_bytes) {
                    result.status = ESP_ERR_INVALID_SIZE;
                    break;
                }
            }
            if (frame_complete(&state->frame, result.data, result.size)) return result;
            continue;
        }
        int64_t remaining = deadline - esp_timer_get_time();
        if (command->timeout_ms > 0 && remaining <= 0) {
            result.status = ESP_ERR_TIMEOUT;
            break;
        }
        TickType_t wait = poll_ticks();
        if (command->timeout_ms > 0) {
            uint32_t wait_ms = (uint32_t)((remaining + 999) / 1000);
            if (wait_ms > UART_WORKER_POLL_MS) wait_ms = UART_WORKER_POLL_MS;
            wait = pdMS_TO_TICKS(wait_ms);
            if (wait == 0) wait = 1;
        }
        uart_event_t event;
        if (xQueueReceive(state->uart_events, &event, wait) != pdTRUE) {
            /* ESP-IDF may omit RX timeout after a FIFO-full event until the
               next byte. Finish that exact-size idle frame after a software
               gap, provided neither hardware FIFO nor driver ring holds data. */
            if (state->frame.type == UART_FRAME_IDLE && result.size > 0 &&
                last_data_event_us > 0 &&
                esp_timer_get_time() - last_data_event_us >= state->frame.gap_us) {
                size_t buffered = 0;
                if (uart_get_buffered_data_len(state->id, &buffered) == ESP_OK &&
                    buffered == 0 &&
                    uart_ll_get_rxfifo_len(UART_LL_GET_HW(state->id)) == 0) {
                    return result;
                }
            }
            continue;
        }
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
            result.status = event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR
                ? ESP_FAIL : ESP_ERR_INVALID_SIZE;
            break;
        }
        if (event.type != UART_DATA) continue;
        state->event_bytes_remaining = event.size;
        if (event.size > 0) last_data_event_us = esp_timer_get_time();
        /* A DATA event with timeout_flag ends an idle-delimited frame. Its
           bytes must be consumed before the boundary is delivered. */
        if (event.timeout_flag && state->frame.type == UART_FRAME_IDLE) {
            while (state->event_bytes_remaining > 0) {
                uint8_t byte = 0;
                if (uart_read_bytes(state->id, &byte, 1, 0) != 1) {
                    result.status = ESP_FAIL;
                    break;
                }
                --state->event_bytes_remaining;
                if (result.size >= state->frame.max_bytes) {
                    result.status = ESP_ERR_INVALID_SIZE;
                    break;
                }
                result.data[result.size++] = byte;
            }
            if (result.status != ESP_OK) break;
            if (result.size > 0) return result;
        }
    }
    if (state_is_closing(state)) result.status = ESP_ERR_INVALID_STATE;
    if ((result.status != ESP_OK && result.status != ESP_ERR_TIMEOUT) || result.size != 0) {
        /* A partial/oversized frame has no trustworthy next boundary. */
        uart_flush_input(state->id);
        xQueueReset(state->uart_events);
        state->event_bytes_remaining = 0;
        if (result.status == ESP_OK) result.status = ESP_ERR_TIMEOUT;
    }
    return result;
}

static uart_completion_t execute_write(uart_port_state_t *state,
                                       uart_command_t *command)
{
    uart_completion_t result = {
        .id = command->id,
        .generation = command->generation,
        .type = command->type,
        .status = ESP_OK,
    };
    int written = uart_write_bytes(state->id, command->data, command->size);
    if (written < 0 || (size_t)written != command->size) {
        result.status = ESP_FAIL;
    } else {
        while (!state_is_closing(state)) {
            esp_err_t err = uart_wait_tx_done(state->id, poll_ticks());
            if (err == ESP_OK) break;
            if (err != ESP_ERR_TIMEOUT) {
                result.status = err;
                break;
            }
        }
        if (state_is_closing(state)) result.status = ESP_ERR_INVALID_STATE;
    }
    result.size = result.status == ESP_OK ? command->size : 0;
    free(command->data);
    command->data = NULL;
    return result;
}

static void uart_worker(void *arg)
{
    uart_port_state_t *state = arg;
    while (!state_is_closing(state)) {
        uart_command_t command = {0};
        if (xQueueReceive(state->commands, &command, poll_ticks()) == pdTRUE) {
            uart_completion_t completion = command.type == UART_OPERATION_READ
                ? execute_read(state, &command)
                : command.type == UART_OPERATION_READ_FRAME
                    ? execute_read_frame(state, &command)
                    : execute_write(state, &command);
            free(command.data);
            xQueueSend(state->completions, &completion, portMAX_DELAY);
            post_completion_wake(state);
        } else if (uxQueueMessagesWaiting(state->completions) > 0) {
            post_completion_wake(state);
        }
    }
    xSemaphoreGive(state->stopped);
    vTaskDelete(NULL);
}

static JSValue make_error(JSContext *ctx, const char *message)
{
    JSValue error = JS_NewError(ctx);
    if (JS_IsException(error)) return error;
    if (JS_SetPropertyStr(ctx, error, "message",
                          JS_NewString(ctx, message)) < 0) {
        JS_FreeValue(ctx, error);
        return JS_EXCEPTION;
    }
    return error;
}

static uart_request_t *find_request(uart_port_state_t *state, uint32_t id,
                                    uint32_t generation)
{
    return js_request_find(&state->requests, id, generation);
}

static void reject_request(uart_port_state_t *state, uart_request_t *request,
                           const char *message)
{
    JSContext *ctx = state->module->ctx;
    JSValue error = make_error(ctx, message);
    if (JS_IsException(error)) error = JS_GetException(ctx);
    js_request_reject(&state->requests, request->id, request->generation, error);
    JS_FreeValue(ctx, error);
}

static JSValue uint8_array_from_bytes(JSContext *ctx, const uint8_t *data,
                                      size_t size)
{
    JSValue buffer = JS_NewArrayBufferCopy(ctx, data, size);
    if (JS_IsException(buffer)) return buffer;
    JSValue array = JS_NewTypedArray(ctx, 1, &buffer, JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, buffer);
    return array;
}

static void complete_request(uart_port_state_t *state,
                             uart_completion_t *completion)
{
    uart_request_t *request = find_request(state, completion->id,
                                           completion->generation);
    if (!request) return;
    if (completion->status != ESP_OK) {
        if (completion->status == ESP_ERR_TIMEOUT) {
            JSContext *ctx = state->module->ctx;
            JSValue error = make_error(ctx, "UART read timeout");
            if (!JS_IsException(error) &&
                (JS_SetPropertyStr(ctx, error, "code", JS_NewString(ctx, "TIMEOUT")) < 0 ||
                 JS_SetPropertyStr(ctx, error, "operation", JS_NewString(ctx,
                     completion->type == UART_OPERATION_READ_FRAME ? "readFrame" : "read")) < 0)) {
                JS_FreeValue(ctx, error);
                error = JS_EXCEPTION;
            }
            if (JS_IsException(error)) error = JS_GetException(ctx);
            js_request_reject(&state->requests, request->id, request->generation, error);
            JS_FreeValue(ctx, error);
            return;
        }
        reject_request(state, request,
                       completion->status == ESP_ERR_INVALID_STATE
                           ? "UART closed"
                           : esp_err_to_name(completion->status));
        return;
    }

    JSContext *ctx = state->module->ctx;
    JSValue value = completion->type != UART_OPERATION_WRITE
        ? uint8_array_from_bytes(ctx, completion->data, completion->size)
        : JS_NewInt32(ctx, (int32_t)completion->size);
    if (JS_IsException(value)) {
        clear_exception(ctx);
        reject_request(state, request, "out of memory");
        return;
    }
    js_request_resolve(&state->requests, request->id, request->generation, value);
    JS_FreeValue(ctx, value);
}

static void uart_event_handler(js_runtime_t *runtime,
                               const js_event_t *event, void *opaque)
{
    (void)runtime;
    js_uart_module_t *module = opaque;
    if (event->source >= UART_NUM_MAX) return;
    uart_port_state_t *state = &module->ports[event->source];
    if (!state->completions) return;
    uart_completion_t completion;
    while (xQueueReceive(state->completions, &completion, 0) == pdTRUE) {
        complete_request(state, &completion);
        free(completion.data);
    }
}

static void reject_all_requests(uart_port_state_t *state, const char *message)
{
    while (state->requests.head) reject_request(state, state->requests.head, message);
}

static void close_port(uart_port_state_t *state, bool reject_promises)
{
    if (!state->open) return;
    portENTER_CRITICAL(&state->lock);
    state->closing = true;
    portEXIT_CRITICAL(&state->lock);
    if (state->task) {
        xTaskNotifyGive(state->task);
        if (state->stopped) xSemaphoreTake(state->stopped, portMAX_DELAY);
    }
    state->task = NULL;

    if (reject_promises) reject_all_requests(state, "UART closed");

    uart_command_t command;
    while (state->commands &&
           xQueueReceive(state->commands, &command, 0) == pdTRUE) {
        free(command.data);
    }
    uart_completion_t completion;
    while (state->completions &&
           xQueueReceive(state->completions, &completion, 0) == pdTRUE) {
        free(completion.data);
    }

    uart_driver_delete(state->id);
    if (state->manually_reserved_pin_mask) {
        esp_gpio_revoke(state->manually_reserved_pin_mask);
    }
    if (state->commands) vQueueDelete(state->commands);
    if (state->completions) vQueueDelete(state->completions);
    if (state->stopped) vSemaphoreDelete(state->stopped);
    state->commands = NULL;
    state->completions = NULL;
    state->uart_events = NULL;
    state->stopped = NULL;
    state->manually_reserved_pin_mask = 0;
    state->tx_pin = UART_PIN_NO_CHANGE;
    state->rx_pin = UART_PIN_NO_CHANGE;
    state->frame = (uart_frame_config_t) {0};
    state->event_bytes_remaining = 0;
    state->requests.count = 0;
    state->open = false;
    portENTER_CRITICAL(&state->lock);
    state->closing = false;
    portEXIT_CRITICAL(&state->lock);
}

static bool set_property(JSContext *ctx, JSValue object, const char *name,
                         JSValue value)
{
    return JS_SetPropertyStr(ctx, object, name, value) >= 0;
}

static JSValue js_uart_list(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    for (int id = 0; id < UART_NUM_MAX; ++id) {
        uart_port_state_t *state = &s_uart.ports[id];
        bool allowed = port_allowed(&s_uart, (uart_port_t)id);
        bool console = port_is_console((uart_port_t)id);
        bool external = !state->open && uart_is_driver_installed(id);
        bool available = allowed && !console && !external;
        JSValue item = JS_NewObject(ctx);
        if (JS_IsException(item) ||
            !set_property(ctx, item, "id", JS_NewInt32(ctx, id)) ||
            !set_property(ctx, item, "available",
                          JS_NewBool(ctx, available)) ||
            !set_property(ctx, item, "open", JS_NewBool(ctx, state->open))) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
        if (!available) {
            const char *reason = !allowed ? "not-allowed"
                                : console ? "console" : "reserved";
            if (!set_property(ctx, item, "reason",
                              JS_NewString(ctx, reason))) {
                JS_FreeValue(ctx, item);
                JS_FreeValue(ctx, array);
                return JS_EXCEPTION;
            }
        }
        if (JS_SetPropertyUint32(ctx, array, (uint32_t)id, item) < 0) {
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

static bool decode_handle(JSContext *ctx, JSValueConst *func_data,
                          uart_port_state_t **out)
{
    uint32_t token = 0;
    if (JS_ToUint32(ctx, &token, func_data[0])) return false;
    uint32_t id = token & UINT32_C(0xff);
    uint32_t generation = token >> 8;
    if (id >= UART_NUM_MAX) {
        JS_ThrowInternalError(ctx, "invalid UART handle");
        return false;
    }
    uart_port_state_t *state = &s_uart.ports[id];
    if (!state->open || state->generation != generation) {
        JS_ThrowInternalError(ctx, "UART closed");
        return false;
    }
    *out = state;
    return true;
}

static JSValue make_promise_request(JSContext *ctx,
                                    uart_port_state_t *state,
                                    uart_command_t *command)
{
    (void)ctx;
    JSValue promise = js_request_create(&state->requests, state->generation,
                                        NULL, NULL, NULL, &command->id);
    if (JS_IsException(promise)) return promise;
    command->generation = state->generation;
    if (command->type != UART_OPERATION_WRITE && command->timeout_ms > 0) {
        command->deadline_us = esp_timer_get_time() + (int64_t)command->timeout_ms * 1000;
    }
    if (xQueueSend(state->commands, command, 0) != pdTRUE) {
        uart_request_t *request = find_request(state, command->id, command->generation);
        if (request) reject_request(state, request, "UART operation queue full");
        free(command->data);
        command->data = NULL;
    }
    return promise;
}

static bool copy_write_data(JSContext *ctx, JSValueConst value,
                            uint8_t **out_data, size_t *out_size)
{
    const uint8_t *source = NULL;
    size_t size = 0;
    const char *string = NULL;
    JSValue buffer = JS_UNDEFINED;
    size_t offset = 0;
    size_t element_size = 0;

    if (JS_IsString(value)) {
        string = JS_ToCStringLen(ctx, &size, value);
        if (!string) return false;
        source = (const uint8_t *)string;
    } else {
        source = JS_GetArrayBuffer(ctx, &size, value);
        if (!source) {
            clear_exception(ctx);
            buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &size,
                                            &element_size);
            if (JS_IsException(buffer)) {
                clear_exception(ctx);
                JS_ThrowTypeError(ctx,
                    "data must be a string, Uint8Array, or ArrayBuffer");
                return false;
            }
            if (element_size != 1) {
                JS_FreeValue(ctx, buffer);
                JS_ThrowTypeError(ctx, "typed array must contain bytes");
                return false;
            }
            size_t buffer_size = 0;
            uint8_t *base = JS_GetArrayBuffer(ctx, &buffer_size, buffer);
            if (!base || offset > buffer_size || size > buffer_size - offset) {
                JS_FreeValue(ctx, buffer);
                JS_ThrowTypeError(ctx, "invalid typed array");
                return false;
            }
            source = base + offset;
        }
    }
    if (size == 0 || size > CONFIG_JS_UART_MAX_TRANSFER_SIZE) {
        JS_FreeCString(ctx, string);
        JS_FreeValue(ctx, buffer);
        JS_ThrowRangeError(ctx, "UART write size must be between 1 and %d",
                           CONFIG_JS_UART_MAX_TRANSFER_SIZE);
        return false;
    }
    uint8_t *copy = malloc(size);
    if (!copy) {
        JS_FreeCString(ctx, string);
        JS_FreeValue(ctx, buffer);
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    memcpy(copy, source, size);
    JS_FreeCString(ctx, string);
    JS_FreeValue(ctx, buffer);
    *out_data = copy;
    *out_size = size;
    return true;
}

static JSValue js_port_write(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv, int magic,
                             JSValue *func_data)
{
    (void)this_value;
    (void)magic;
    uart_port_state_t *state = NULL;
    if (!decode_handle(ctx, func_data, &state)) return JS_EXCEPTION;
    if (state->tx_pin == UART_PIN_NO_CHANGE) {
        return JS_ThrowTypeError(ctx, "UART TX is not configured");
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "data is required");
    uint8_t *data = NULL;
    size_t size = 0;
    if (!copy_write_data(ctx, argv[0], &data, &size)) return JS_EXCEPTION;
    uart_command_t command = {
        .type = UART_OPERATION_WRITE,
        .data = data,
        .size = size,
    };
    if (state->requests.count >= CONFIG_JS_UART_MAX_PENDING) {
        free(data);
        JSValue error = make_error(ctx, "UART operation queue full");
        if (JS_IsException(error)) return error;
        JSValue promise = js_request_promise(ctx, true, error);
        JS_FreeValue(ctx, error);
        return promise;
    }
    JSValue promise = make_promise_request(ctx, state, &command);
    if (JS_IsException(promise)) free(data);
    return promise;
}

static JSValue js_port_read(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv, int magic,
                            JSValue *func_data)
{
    (void)this_value;
    (void)magic;
    uart_port_state_t *state = NULL;
    if (!decode_handle(ctx, func_data, &state)) return JS_EXCEPTION;
    if (state->rx_pin == UART_PIN_NO_CHANGE) {
        return JS_ThrowTypeError(ctx, "UART RX is not configured");
    }
    if (state->frame.type != UART_FRAME_NONE) {
        return JS_ThrowTypeError(ctx, "use readFrame() on a framed UART");
    }
    int32_t max_bytes = 0;
    int32_t timeout_ms = 0;
    if (argc < 1 ||
        !get_integer(ctx, argv[0], "maxBytes", 1,
                     CONFIG_JS_UART_MAX_TRANSFER_SIZE, &max_bytes) ||
        !parse_read_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &timeout_ms)) {
        return JS_EXCEPTION;
    }
    uint8_t *data = malloc((size_t)max_bytes);
    if (!data) return JS_ThrowOutOfMemory(ctx);
    uart_command_t command = {
        .type = UART_OPERATION_READ,
        .data = data,
        .size = (size_t)max_bytes,
        .timeout_ms = (uint32_t)timeout_ms,
    };
    if (state->requests.count >= CONFIG_JS_UART_MAX_PENDING) {
        free(data);
        JSValue error = make_error(ctx, "UART operation queue full");
        if (JS_IsException(error)) return error;
        JSValue promise = js_request_promise(ctx, true, error);
        JS_FreeValue(ctx, error);
        return promise;
    }
    JSValue promise = make_promise_request(ctx, state, &command);
    if (JS_IsException(promise)) free(data);
    return promise;
}

static JSValue js_port_read_frame(JSContext *ctx, JSValueConst this_value,
                                   int argc, JSValueConst *argv, int magic,
                                   JSValue *func_data)
{
    (void)this_value;
    (void)magic;
    uart_port_state_t *state = NULL;
    if (!decode_handle(ctx, func_data, &state)) return JS_EXCEPTION;
    if (state->frame.type == UART_FRAME_NONE) {
        return JS_ThrowTypeError(ctx, "framing must be configured at uart.open()");
    }
    int32_t timeout_ms = 0;
    if (!parse_read_options(ctx, argc > 0 ? argv[0] : JS_UNDEFINED, &timeout_ms)) {
        return JS_EXCEPTION;
    }
    uint8_t *data = malloc(state->frame.max_bytes);
    if (!data) return JS_ThrowOutOfMemory(ctx);
    uart_command_t command = {
        .type = UART_OPERATION_READ_FRAME,
        .data = data,
        .size = state->frame.max_bytes,
        .timeout_ms = (uint32_t)timeout_ms,
    };
    if (state->requests.count >= CONFIG_JS_UART_MAX_PENDING) {
        free(data);
        JSValue error = make_error(ctx, "UART operation queue full");
        if (JS_IsException(error)) return error;
        JSValue promise = js_request_promise(ctx, true, error);
        JS_FreeValue(ctx, error);
        return promise;
    }
    JSValue promise = make_promise_request(ctx, state, &command);
    if (JS_IsException(promise)) free(data);
    return promise;
}

static JSValue js_port_close(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv, int magic,
                             JSValue *func_data)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    (void)magic;
    uint32_t token = 0;
    if (JS_ToUint32(ctx, &token, func_data[0])) return JS_EXCEPTION;
    uint32_t id = token & UINT32_C(0xff);
    uint32_t generation = token >> 8;
    if (id < UART_NUM_MAX) {
        uart_port_state_t *state = &s_uart.ports[id];
        if (state->open && state->generation == generation) {
            close_port(state, true);
        }
    }
    return JS_UNDEFINED;
}

static bool add_port_method(JSContext *ctx, JSValue object, const char *name,
                            JSCFunctionData *function, int argc,
                            uint32_t token)
{
    JSValue data = JS_NewUint32(ctx, token);
    JSValue method = JS_NewCFunctionData(ctx, function, argc, 0, 1, &data);
    JS_FreeValue(ctx, data);
    if (JS_IsException(method)) return false;
    return JS_SetPropertyStr(ctx, object, name, method) >= 0;
}

static JSValue js_uart_open(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    uart_port_t id;
    if (!get_port_id(ctx, argc, argv, &id)) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "UART options are required");
    uart_open_options_t options;
    if (!parse_open_options(ctx, argv[1], &options)) return JS_EXCEPTION;
    uart_port_state_t *state = &s_uart.ports[id];
    if (!port_allowed(&s_uart, id)) {
        return JS_ThrowRangeError(ctx, "UART is not allowed");
    }
    if (port_is_console(id)) {
        return JS_ThrowTypeError(ctx, "UART is used by the console");
    }
    if (state->open || uart_is_driver_installed(id)) {
        return JS_ThrowTypeError(ctx, "UART already open");
    }

    uint64_t pin_mask = 0;
    if (options.tx != UART_PIN_NO_CHANGE) pin_mask |= BIT64(options.tx);
    if (options.rx != UART_PIN_NO_CHANGE) pin_mask |= BIT64(options.rx);
    if (options.direction_pin != UART_PIN_NO_CHANGE) pin_mask |= BIT64(options.direction_pin);
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if (!(pin_mask & BIT64(pin))) continue;
        if (!pin_allowed(&s_uart, pin)) {
            return JS_ThrowRangeError(ctx, "UART pin %d is not allowed", pin);
        }
    }
    if (esp_gpio_is_reserved(pin_mask)) {
        return JS_ThrowTypeError(ctx, "UART pin is reserved");
    }

    uart_config_t driver_config = {
        .baud_rate = options.baud_rate,
        .data_bits = options.data_bits,
        .parity = options.parity,
        .stop_bits = options.stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t err = uart_param_config(id, &driver_config);
    if (err == ESP_OK) {
        err = uart_driver_install(id, options.rx_buffer_size,
                                  options.tx_buffer_size,
                                  options.frame.type == UART_FRAME_NONE ? 0 : 20,
                                  options.frame.type == UART_FRAME_NONE ? NULL : &state->uart_events,
                                  0);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(id, options.tx, options.rx,
                           options.direction_pin, UART_PIN_NO_CHANGE);
    }
    if (err == ESP_OK && options.direction_pin != UART_PIN_NO_CHANGE) {
        err = uart_set_mode(id, UART_MODE_RS485_HALF_DUPLEX);
    }
    if (err == ESP_OK && options.frame.type != UART_FRAME_NONE) {
        err = uart_set_rx_timeout(id,
            options.frame.type == UART_FRAME_IDLE ? options.frame.rx_timeout_chars : 2);
        if (err == ESP_OK) {
            int threshold = options.frame.max_bytes < 126
                ? (int)options.frame.max_bytes + 1 : 126;
            err = uart_set_rx_full_threshold(id, threshold);
        }
    }
    if (err != ESP_OK) {
        if (uart_is_driver_installed(id)) uart_driver_delete(id);
        state->uart_events = NULL;
        return throw_esp_error(ctx, "uart.open", err);
    }
    /* UART reserves routed outputs itself; reserve any remaining input pins. */
    uint64_t already_reserved = esp_gpio_reserve(pin_mask);
    uint64_t manually_reserved = pin_mask & ~already_reserved;

    state->commands = xQueueCreate(CONFIG_JS_UART_MAX_PENDING,
                                    sizeof(uart_command_t));
    state->completions = xQueueCreate(CONFIG_JS_UART_MAX_PENDING,
                                      sizeof(uart_completion_t));
    state->stopped = xSemaphoreCreateBinary();
    if (!state->commands || !state->completions || !state->stopped) {
        state->open = true;
        state->manually_reserved_pin_mask = manually_reserved;
        close_port(state, false);
        return JS_ThrowOutOfMemory(ctx);
    }
    state->generation = (state->generation + 1) & UINT32_C(0x00ffffff);
    if (state->generation == 0) state->generation = 1;
    state->manually_reserved_pin_mask = manually_reserved;
    state->tx_pin = options.tx;
    state->rx_pin = options.rx;
    state->frame = options.frame;
    state->event_bytes_remaining = 0;
    state->open = true;
    state->closing = false;
    BaseType_t created = xTaskCreate(uart_worker, "js_uart",
        CONFIG_JS_UART_TASK_STACK_SIZE, state, CONFIG_JS_UART_TASK_PRIORITY,
        &state->task);
    if (created != pdPASS) {
        close_port(state, false);
        return JS_ThrowOutOfMemory(ctx);
    }

    uint32_t token = (state->generation << 8) | (uint32_t)id;
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object) ||
        !set_property(ctx, object, "id", JS_NewInt32(ctx, id)) ||
        !add_port_method(ctx, object, "write", js_port_write, 1, token) ||
        !add_port_method(ctx, object, "read", js_port_read, 2, token) ||
        !add_port_method(ctx, object, "readFrame", js_port_read_frame, 1, token) ||
        !add_port_method(ctx, object, "close", js_port_close, 0, token)) {
        JS_FreeValue(ctx, object);
        close_port(state, false);
        return JS_EXCEPTION;
    }
    return object;
}

static esp_err_t install_method(JSContext *ctx, JSValue object,
                                const char *name, JSCFunction *function,
                                int argc)
{
    JSValue value = JS_NewCFunction(ctx, function, name, argc);
    if (JS_IsException(value)) return ESP_ERR_NO_MEM;
    return JS_SetPropertyStr(ctx, object, name, value) < 0
               ? ESP_FAIL : ESP_OK;
}

static esp_err_t uart_module_init(js_runtime_t *runtime, JSContext *ctx,
                                  void *opaque)
{
    js_uart_module_t *module = opaque;
    module->runtime = runtime;
    module->ctx = ctx;
    for (int id = 0; id < UART_NUM_MAX; ++id) {
        uart_port_state_t *state = &module->ports[id];
        state->module = module;
        js_request_table_init(&state->requests, ctx);
        esp_err_t tracked = js_runtime_track_requests(runtime, &state->requests);
        if (tracked != ESP_OK) return tracked;
        state->id = (uart_port_t)id;
        state->tx_pin = UART_PIN_NO_CHANGE;
        state->rx_pin = UART_PIN_NO_CHANGE;
        state->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    }
    esp_err_t err = js_event_register_handler(runtime, JS_EVENT_UART,
                                               uart_event_handler, module);
    if (err != ESP_OK) return err;

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue uart = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(uart)) {
        JS_FreeValue(ctx, uart);
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_UART);
        return ESP_ERR_NO_MEM;
    }
    if (install_method(ctx, uart, "list", js_uart_list, 0) != ESP_OK ||
        install_method(ctx, uart, "open", js_uart_open, 2) != ESP_OK) {
        JS_FreeValue(ctx, uart);
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_UART);
        return ESP_FAIL;
    }
    if (JS_SetPropertyStr(ctx, global, "uart", uart) < 0) {
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_UART);
        return ESP_FAIL;
    }
    JS_FreeValue(ctx, global);
    module->initialized = true;
    return ESP_OK;
}

static void uart_module_deinit(js_runtime_t *runtime, JSContext *ctx,
                               void *opaque)
{
    (void)ctx;
    js_uart_module_t *module = opaque;
    module->initialized = false;
    for (int id = 0; id < UART_NUM_MAX; ++id) {
        if (module->ports[id].open) close_port(&module->ports[id], true);
    }
    js_event_unregister_handler(runtime, JS_EVENT_UART);
    module->runtime = NULL;
    module->ctx = NULL;
}

esp_err_t js_uart_register(const js_uart_config_t *config)
{
    if (!config || config->allowed_port_mask == 0 ||
        config->allowed_pin_mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registered) return ESP_ERR_INVALID_STATE;
    s_uart.allowed_port_mask = config->allowed_port_mask;
    s_uart.allowed_pin_mask = config->allowed_pin_mask;
    esp_err_t err = js_runtime_register_module(uart_module_init,
                                               uart_module_deinit, &s_uart);
    if (err == ESP_OK) s_registered = true;
    return err;
}

#else

esp_err_t js_uart_register(const js_uart_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
