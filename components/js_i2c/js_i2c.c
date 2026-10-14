#include "js_i2c.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "js_event.h"
#include "js_runtime.h"
#include "js_request.h"
#include "quickjs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_JS_I2C_ENABLED

#define I2C_TIMEOUT_DEFAULT_MS 1000
#define I2C_PROBE_TIMEOUT_DEFAULT_MS 10
#define I2C_TIMEOUT_MAX_MS 60000
#define I2C_CLOCK_MAX_HZ 1000000
#define I2C_WORKER_POLL_MS 10

typedef enum {
    I2C_OP_OPEN,
    I2C_OP_DEVICE,
    I2C_OP_WRITE,
    I2C_OP_READ,
    I2C_OP_WRITE_READ,
    I2C_OP_PROBE,
    I2C_OP_SCAN,
    I2C_OP_DEVICE_CLOSE,
    I2C_OP_BUS_CLOSE,
} i2c_operation_t;

typedef struct js_i2c_module js_i2c_module_t;
typedef struct i2c_request i2c_request_t;

typedef struct {
    i2c_master_dev_handle_t handle;
    uint32_t generation;
    uint32_t close_request_id;
    JSValue close_promise;
    uint16_t address;
    uint32_t clock_hz;
    uint8_t address_bits;
    bool active;
    bool closing;
} i2c_device_state_t;

typedef struct {
    uint32_t id;
    uint32_t bus_generation;
    uint32_t device_generation;
    uint16_t device_slot;
    uint16_t address;
    uint16_t scan_from;
    uint16_t scan_to;
    uint32_t timeout_ms;
    int64_t deadline_us;
    i2c_operation_t type;
    i2c_master_dev_handle_t device;
    uint8_t *write_data;
    size_t write_size;
    uint8_t *read_data;
    size_t read_size;
} i2c_command_t;

typedef struct {
    uint32_t id;
    uint32_t bus_generation;
    uint32_t device_generation;
    uint16_t device_slot;
    uint16_t address;
    i2c_operation_t type;
    esp_err_t status;
    uint8_t *data;
    size_t size;
} i2c_completion_t;

struct i2c_request {
    uint32_t id;
    uint32_t bus_generation;
    uint32_t device_generation;
    uint16_t device_slot;
    uint16_t address;
    i2c_operation_t type;
};

typedef struct {
    js_i2c_module_t *module;
    i2c_port_num_t id;
    i2c_master_bus_handle_t handle;
    QueueHandle_t commands;
    QueueHandle_t completions;
    SemaphoreHandle_t stopped;
    TaskHandle_t task;
    js_request_table_t requests;
    i2c_device_state_t devices[CONFIG_JS_I2C_MAX_DEVICES_PER_BUS];
    uint64_t manually_reserved_pin_mask;
    uint32_t generation;
    uint32_t close_request_id;
    JSValue close_promise;
    int sda;
    int scl;
    portMUX_TYPE lock;
    bool open;
    bool closing;
} i2c_bus_state_t;

struct js_i2c_module {
    js_runtime_t *runtime;
    JSContext *ctx;
    uint32_t allowed_port_mask;
    uint64_t allowed_pin_mask;
    i2c_bus_state_t buses[I2C_NUM_MAX];
    JSValue error_constructor;
    volatile bool initialized;
};

typedef struct {
    int sda;
    int scl;
    uint8_t glitch_ignore_cycles;
    bool internal_pullups;
} i2c_open_options_t;

typedef struct {
    uint8_t address_bits;
    uint32_t clock_hz;
    uint32_t clock_stretch_timeout_us;
} i2c_device_options_t;

static js_i2c_module_t s_i2c;
static bool s_registered;

static const char *operation_name(i2c_operation_t type)
{
    switch (type) {
        case I2C_OP_OPEN: return "open";
        case I2C_OP_DEVICE: return "device";
        case I2C_OP_WRITE: return "write";
        case I2C_OP_READ: return "read";
        case I2C_OP_WRITE_READ: return "writeRead";
        case I2C_OP_PROBE: return "probe";
        case I2C_OP_SCAN: return "scan";
        case I2C_OP_DEVICE_CLOSE:
        case I2C_OP_BUS_CLOSE: return "close";
        default: return "unknown";
    }
}

static bool port_allowed(const js_i2c_module_t *module, i2c_port_num_t port)
{
    return port >= 0 && port < 32 &&
           (module->allowed_port_mask & (UINT32_C(1) << port)) != 0;
}

static bool pin_allowed(const js_i2c_module_t *module, int pin)
{
    return pin >= 0 && pin < 64 &&
           (module->allowed_pin_mask & (UINT64_C(1) << pin)) != 0;
}

static bool bus_is_closing(i2c_bus_state_t *state)
{
    portENTER_CRITICAL(&state->lock);
    bool closing = state->closing;
    portEXIT_CRITICAL(&state->lock);
    return closing;
}

static void set_bus_closing(i2c_bus_state_t *state, bool closing)
{
    portENTER_CRITICAL(&state->lock);
    state->closing = closing;
    portEXIT_CRITICAL(&state->lock);
}

static void clear_exception(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
}

static bool set_property(JSContext *ctx, JSValue object, const char *name,
                         JSValue value)
{
    return JS_SetPropertyStr(ctx, object, name, value) >= 0;
}

static bool set_readonly_property(JSContext *ctx, JSValue object,
                                  const char *name, JSValue value)
{
    return JS_DefinePropertyValueStr(ctx, object, name, value,
                                     JS_PROP_ENUMERABLE) >= 0;
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

static bool get_optional_integer(JSContext *ctx, JSValueConst options,
                                 const char *name, int32_t min, int32_t max,
                                 int32_t *out, bool *present)
{
    JSValue value = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsException(value)) return false;
    if (JS_IsUndefined(value)) {
        *present = false;
        JS_FreeValue(ctx, value);
        return true;
    }
    bool ok = get_integer(ctx, value, name, min, max, out);
    JS_FreeValue(ctx, value);
    *present = ok;
    return ok;
}

static bool validate_fields(JSContext *ctx, JSValueConst object,
                            const char *const *known, size_t known_count,
                            const char *kind)
{
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, object,
                               JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return false;
    }
    bool valid = true;
    for (uint32_t i = 0; i < count && valid; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) {
            valid = false;
            break;
        }
        bool found = false;
        for (size_t j = 0; j < known_count; ++j) {
            if (strcmp(name, known[j]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
            JS_ThrowTypeError(ctx, "unknown I2C %s option: %s", kind, name);
            valid = false;
        }
        JS_FreeCString(ctx, name);
    }
    JS_FreePropertyEnum(ctx, properties, count);
    return valid;
}

static bool options_object(JSContext *ctx, JSValueConst value,
                           const char *kind)
{
    if (!JS_IsObject(value) || JS_IsNull(value) || JS_IsArray(ctx, value) ||
        JS_IsFunction(ctx, value)) {
        JS_ThrowTypeError(ctx, "I2C %s options must be an object", kind);
        return false;
    }
    return true;
}

static bool parse_open_options(JSContext *ctx, JSValueConst value,
                               i2c_open_options_t *out)
{
    static const char *const fields[] = {
        "sda", "scl", "pullups", "glitchIgnoreCycles",
    };
    if (!options_object(ctx, value, "bus") ||
        !validate_fields(ctx, value, fields, 4, "bus")) return false;
    *out = (i2c_open_options_t) {
        .sda = -1,
        .scl = -1,
        .glitch_ignore_cycles = 7,
    };
    bool sda_present = false;
    bool scl_present = false;
    bool present = false;
    int32_t parsed = 0;
    if (!get_optional_integer(ctx, value, "sda", 0, GPIO_NUM_MAX - 1,
                              &parsed, &sda_present)) return false;
    out->sda = parsed;
    if (!get_optional_integer(ctx, value, "scl", 0, GPIO_NUM_MAX - 1,
                              &parsed, &scl_present)) return false;
    out->scl = parsed;
    if (!get_optional_integer(ctx, value, "glitchIgnoreCycles", 0, UINT8_MAX,
                              &parsed, &present)) return false;
    if (present) out->glitch_ignore_cycles = (uint8_t)parsed;
    if (!sda_present || !scl_present) {
        JS_ThrowTypeError(ctx, "sda and scl are required");
        return false;
    }
    if (!GPIO_IS_VALID_GPIO(out->sda) || !GPIO_IS_VALID_OUTPUT_GPIO(out->sda)) {
        JS_ThrowRangeError(ctx, "invalid I2C SDA pin");
        return false;
    }
    if (!GPIO_IS_VALID_GPIO(out->scl) || !GPIO_IS_VALID_OUTPUT_GPIO(out->scl)) {
        JS_ThrowRangeError(ctx, "invalid I2C SCL pin");
        return false;
    }
    if (out->sda == out->scl) {
        JS_ThrowRangeError(ctx, "sda and scl must use different pins");
        return false;
    }
    JSValue pullups = JS_GetPropertyStr(ctx, value, "pullups");
    if (JS_IsException(pullups)) return false;
    if (!JS_IsUndefined(pullups)) {
        if (!JS_IsString(pullups)) {
            JS_FreeValue(ctx, pullups);
            JS_ThrowTypeError(ctx, "pullups must be a string");
            return false;
        }
        const char *text = JS_ToCString(ctx, pullups);
        JS_FreeValue(ctx, pullups);
        if (!text) return false;
        if (strcmp(text, "internal") == 0) out->internal_pullups = true;
        else if (strcmp(text, "external") != 0) {
            JS_FreeCString(ctx, text);
            JS_ThrowRangeError(ctx, "pullups must be external or internal");
            return false;
        }
        JS_FreeCString(ctx, text);
    } else {
        JS_FreeValue(ctx, pullups);
    }
    return true;
}

static bool parse_device_options(JSContext *ctx, JSValueConst value,
                                 i2c_device_options_t *out)
{
    static const char *const fields[] = {
        "addressBits", "clockHz", "clockStretchTimeoutUs",
    };
    *out = (i2c_device_options_t) {
        .address_bits = 7,
        .clock_hz = 100000,
    };
    if (JS_IsUndefined(value)) return true;
    if (!options_object(ctx, value, "device") ||
        !validate_fields(ctx, value, fields, 3, "device")) return false;
    bool present = false;
    int32_t parsed = 0;
    if (!get_optional_integer(ctx, value, "addressBits", 7, 10,
                              &parsed, &present)) return false;
    if (present && parsed != 7 && parsed != 10) {
        JS_ThrowRangeError(ctx, "addressBits must be 7 or 10");
        return false;
    }
    if (present) out->address_bits = (uint8_t)parsed;
    if (!get_optional_integer(ctx, value, "clockHz", 1, I2C_CLOCK_MAX_HZ,
                              &parsed, &present)) return false;
    if (present) out->clock_hz = (uint32_t)parsed;
    if (!get_optional_integer(ctx, value, "clockStretchTimeoutUs", 0,
                              INT32_MAX, &parsed, &present)) return false;
    if (present) out->clock_stretch_timeout_us = (uint32_t)parsed;
    return true;
}

static bool parse_operation_options(JSContext *ctx, int argc,
                                    JSValueConst *argv, int index,
                                    uint32_t default_ms, uint32_t *out)
{
    *out = default_ms;
    if (argc <= index || JS_IsUndefined(argv[index])) return true;
    static const char *const fields[] = {"timeoutMs"};
    if (!options_object(ctx, argv[index], "operation") ||
        !validate_fields(ctx, argv[index], fields, 1, "operation")) {
        return false;
    }
    bool present = false;
    int32_t parsed = 0;
    if (!get_optional_integer(ctx, argv[index], "timeoutMs", 0,
                              I2C_TIMEOUT_MAX_MS, &parsed, &present)) {
        return false;
    }
    if (present) *out = (uint32_t)parsed;
    return true;
}

static JSValue make_i2c_error(JSContext *ctx, const char *code,
                              const char *message, i2c_operation_t operation,
                              int bus_id, int address)
{
    JSValue args[5] = {
        JS_NewString(ctx, code),
        JS_NewString(ctx, message),
        JS_NewString(ctx, operation_name(operation)),
        JS_NewInt32(ctx, bus_id),
        address >= 0 ? JS_NewInt32(ctx, address) : JS_UNDEFINED,
    };
    JSValue error = JS_CallConstructor(ctx, s_i2c.error_constructor, 5,
                                       (JSValueConst *)args);
    for (size_t i = 0; i < 5; ++i) JS_FreeValue(ctx, args[i]);
    return error;
}

static JSValue throw_i2c_error(JSContext *ctx, const char *code,
                               const char *message, i2c_operation_t operation,
                               int bus_id, int address)
{
    JSValue error = make_i2c_error(ctx, code, message, operation, bus_id,
                                   address);
    if (JS_IsException(error)) return error;
    return JS_Throw(ctx, error);
}

static const char *error_code(esp_err_t status)
{
    if (status == ESP_ERR_INVALID_RESPONSE || status == ESP_ERR_NOT_FOUND) {
        return "NACK";
    }
    if (status == ESP_ERR_TIMEOUT) return "TIMEOUT";
    if (status == ESP_ERR_INVALID_STATE) return "CLOSED";
    return "IO";
}

static JSValue resolved_promise(JSContext *ctx)
{
    return js_request_promise(ctx, false, JS_UNDEFINED);
}

static JSValue rejected_promise(JSContext *ctx, const char *code,
                                const char *message,
                                i2c_operation_t operation, int bus_id,
                                int address)
{
    JSValue error = make_i2c_error(ctx, code, message, operation, bus_id, address);
    if (JS_IsException(error)) return error;
    JSValue promise = js_request_promise(ctx, true, error);
    JS_FreeValue(ctx, error);
    return promise;
}

static bool copy_data(JSContext *ctx, JSValueConst value,
                      uint8_t **out_data, size_t *out_size)
{
    const uint8_t *source = NULL;
    size_t size = 0;
    size_t offset = 0;
    size_t element_size = 0;
    JSValue buffer = JS_UNDEFINED;
    source = JS_GetArrayBuffer(ctx, &size, value);
    if (!source) {
        clear_exception(ctx);
        buffer = JS_GetTypedArrayBuffer(ctx, value, &offset, &size,
                                        &element_size);
        if (JS_IsException(buffer)) {
            clear_exception(ctx);
            JS_ThrowTypeError(ctx, "data must be a Uint8Array or ArrayBuffer");
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
    if (size == 0 || size > CONFIG_JS_I2C_MAX_TRANSFER_SIZE) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowRangeError(ctx, "I2C data size must be between 1 and %d",
                           CONFIG_JS_I2C_MAX_TRANSFER_SIZE);
        return false;
    }
    uint8_t *copy = malloc(size);
    if (!copy) {
        JS_FreeValue(ctx, buffer);
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    memcpy(copy, source, size);
    JS_FreeValue(ctx, buffer);
    *out_data = copy;
    *out_size = size;
    return true;
}

static esp_err_t post_completion_wake(i2c_bus_state_t *state)
{
    js_event_t event = {
        .type = JS_EVENT_I2C,
        .source = (uint32_t)state->id,
        .request_id = state->generation,
    };
    return js_event_post(state->module->runtime, &event, 0);
}

static TickType_t worker_poll_ticks(void)
{
    TickType_t ticks = pdMS_TO_TICKS(I2C_WORKER_POLL_MS);
    return ticks > 0 ? ticks : 1;
}

static i2c_completion_t execute_command(i2c_bus_state_t *state,
                                        i2c_command_t *command)
{
    i2c_completion_t result = {
        .id = command->id,
        .bus_generation = command->bus_generation,
        .device_generation = command->device_generation,
        .device_slot = command->device_slot,
        .address = command->address,
        .type = command->type,
        .status = ESP_OK,
    };
    /* ESP-IDF uses -1 for an unlimited software wait. Hardware timeout and
       NACK errors still terminate a transfer. */
    int timeout_ms = command->timeout_ms ? (int)command->timeout_ms : -1;
    if (command->deadline_us) {
        int64_t remaining = command->deadline_us - esp_timer_get_time();
        if (remaining <= 0) {
            result.status = ESP_ERR_TIMEOUT;
            goto done;
        }
        timeout_ms = (int)((remaining + 999) / 1000);
    }
    switch (command->type) {
        case I2C_OP_WRITE:
            result.status = i2c_master_transmit(command->device,
                command->write_data, command->write_size,
                timeout_ms);
            result.size = result.status == ESP_OK ? command->write_size : 0;
            break;
        case I2C_OP_READ:
            result.status = i2c_master_receive(command->device,
                command->read_data, command->read_size,
                timeout_ms);
            result.data = command->read_data;
            result.size = result.status == ESP_OK ? command->read_size : 0;
            command->read_data = NULL;
            break;
        case I2C_OP_WRITE_READ:
            result.status = i2c_master_transmit_receive(command->device,
                command->write_data, command->write_size,
                command->read_data, command->read_size,
                timeout_ms);
            result.data = command->read_data;
            result.size = result.status == ESP_OK ? command->read_size : 0;
            command->read_data = NULL;
            break;
        case I2C_OP_PROBE:
            result.status = i2c_master_probe(state->handle, command->address,
                                              timeout_ms);
            break;
        case I2C_OP_SCAN:
            for (uint16_t address = command->scan_from;
                 address <= command->scan_to; ++address) {
                esp_err_t err = i2c_master_probe(state->handle, address,
                                                 timeout_ms);
                if (err == ESP_OK) command->read_data[result.size++] = address;
                else if (err != ESP_ERR_NOT_FOUND) {
                    result.status = err;
                    result.size = 0;
                    break;
                }
            }
            result.data = command->read_data;
            command->read_data = NULL;
            break;
        case I2C_OP_DEVICE_CLOSE:
            if (command->device) {
                result.status = i2c_master_bus_rm_device(command->device);
                if (command->device_slot < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS) {
                    state->devices[command->device_slot].handle = NULL;
                }
            }
            break;
        default:
            result.status = ESP_ERR_INVALID_STATE;
            break;
    }
done:
    free(command->write_data);
    free(command->read_data);
    command->write_data = NULL;
    command->read_data = NULL;
    return result;
}

static void send_completion(i2c_bus_state_t *state,
                            const i2c_completion_t *completion)
{
    xQueueSend(state->completions, completion, portMAX_DELAY);
    post_completion_wake(state);
}

static void close_native_bus(i2c_bus_state_t *state)
{
    for (size_t i = 0; i < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS; ++i) {
        if (state->devices[i].handle) {
            i2c_master_bus_rm_device(state->devices[i].handle);
            state->devices[i].handle = NULL;
        }
    }
    if (state->handle) {
        i2c_del_master_bus(state->handle);
        state->handle = NULL;
    }
}

static void i2c_worker(void *arg)
{
    i2c_bus_state_t *state = arg;
    while (!bus_is_closing(state)) {
        i2c_command_t command = {0};
        if (xQueueReceive(state->commands, &command,
                          worker_poll_ticks()) == pdTRUE) {
            i2c_completion_t completion;
            if (bus_is_closing(state) &&
                command.type != I2C_OP_DEVICE_CLOSE) {
                completion = (i2c_completion_t) {
                    .id = command.id,
                    .bus_generation = command.bus_generation,
                    .device_generation = command.device_generation,
                    .device_slot = command.device_slot,
                    .address = command.address,
                    .type = command.type,
                    .status = ESP_ERR_INVALID_STATE,
                };
                free(command.write_data);
                free(command.read_data);
            } else {
                completion = execute_command(state, &command);
            }
            send_completion(state, &completion);
        } else if (uxQueueMessagesWaiting(state->completions) > 0) {
            post_completion_wake(state);
        }
    }

    i2c_command_t command;
    while (xQueueReceive(state->commands, &command, 0) == pdTRUE) {
        i2c_completion_t completion = {
            .id = command.id,
            .bus_generation = command.bus_generation,
            .device_generation = command.device_generation,
            .device_slot = command.device_slot,
            .address = command.address,
            .type = command.type,
            .status = command.type == I2C_OP_DEVICE_CLOSE
                          ? ESP_OK : ESP_ERR_INVALID_STATE,
        };
        free(command.write_data);
        free(command.read_data);
        send_completion(state, &completion);
    }
    close_native_bus(state);
    if (state->close_request_id != 0) {
        i2c_completion_t completion = {
            .id = state->close_request_id,
            .bus_generation = state->generation,
            .type = I2C_OP_BUS_CLOSE,
            .status = ESP_OK,
        };
        send_completion(state, &completion);
    }
    xSemaphoreGive(state->stopped);
    while (state->module->initialized &&
           uxQueueMessagesWaiting(state->completions) > 0 &&
           post_completion_wake(state) != ESP_OK) {
        vTaskDelay(1);
    }
    vTaskDelete(NULL);
}

static i2c_request_t *find_request(i2c_bus_state_t *state, uint32_t id,
                                  uint32_t generation)
{
    js_request_t *request = js_request_find(&state->requests, id, generation);
    return request ? request->opaque : NULL;
}

static void reject_request(i2c_bus_state_t *state, i2c_request_t *request,
                           const char *code, const char *message)
{
    JSContext *ctx = state->module->ctx;
    JSValue error = make_i2c_error(ctx, code, message, request->type, state->id,
                                  request->address <= 0x3ff ? request->address : -1);
    if (JS_IsException(error)) error = JS_GetException(ctx);
    js_request_reject(&state->requests, request->id, request->bus_generation, error);
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

static JSValue number_array_from_bytes(JSContext *ctx, const uint8_t *data,
                                       size_t size)
{
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    for (size_t i = 0; i < size; ++i) {
        if (JS_SetPropertyUint32(ctx, array, (uint32_t)i,
                                 JS_NewInt32(ctx, data[i])) < 0) {
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

static void finalize_bus_state(i2c_bus_state_t *state)
{
    JSContext *ctx = state->module->ctx;
    if (!JS_IsUndefined(state->close_promise)) {
        JS_FreeValue(ctx, state->close_promise);
        state->close_promise = JS_UNDEFINED;
    }
    for (size_t i = 0; i < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS; ++i) {
        i2c_device_state_t *device = &state->devices[i];
        if (!JS_IsUndefined(device->close_promise)) {
            JS_FreeValue(ctx, device->close_promise);
            device->close_promise = JS_UNDEFINED;
        }
        device->active = false;
        device->closing = false;
        device->handle = NULL;
        device->close_request_id = 0;
    }
    if (state->manually_reserved_pin_mask) {
        esp_gpio_revoke(state->manually_reserved_pin_mask);
    }
    if (state->commands) vQueueDelete(state->commands);
    if (state->completions) vQueueDelete(state->completions);
    if (state->stopped) vSemaphoreDelete(state->stopped);
    state->commands = NULL;
    state->completions = NULL;
    state->stopped = NULL;
    state->task = NULL;
    state->manually_reserved_pin_mask = 0;
    state->close_request_id = 0;
    state->sda = -1;
    state->scl = -1;
    state->requests.count = 0;
    state->open = false;
    set_bus_closing(state, false);
}

static void complete_request(i2c_bus_state_t *state,
                             i2c_completion_t *completion)
{
    i2c_request_t *request = find_request(state, completion->id,
                                           completion->bus_generation);
    if (!request) return;
    JSContext *ctx = state->module->ctx;
    bool probe_nack = request->type == I2C_OP_PROBE &&
                      completion->status == ESP_ERR_NOT_FOUND;
    if (completion->status != ESP_OK && !probe_nack) {
        if (request->type == I2C_OP_DEVICE_CLOSE &&
            request->device_slot < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS) {
            i2c_device_state_t *device =
                &state->devices[request->device_slot];
            if (device->generation == request->device_generation) {
                if (!JS_IsUndefined(device->close_promise)) {
                    JS_FreeValue(ctx, device->close_promise);
                    device->close_promise = JS_UNDEFINED;
                }
                device->closing = false;
                device->close_request_id = 0;
            }
        }
        const char *message = completion->status == ESP_ERR_INVALID_STATE
            ? "I2C closed" : esp_err_to_name(completion->status);
        reject_request(state, request, error_code(completion->status), message);
        return;
    }

    JSValue value = JS_UNDEFINED;
    if (request->type == I2C_OP_READ ||
        request->type == I2C_OP_WRITE_READ) {
        value = uint8_array_from_bytes(ctx, completion->data,
                                       completion->size);
    } else if (request->type == I2C_OP_WRITE) {
        value = JS_NewInt32(ctx, (int32_t)completion->size);
    } else if (request->type == I2C_OP_PROBE) {
        value = JS_NewBool(ctx, !probe_nack);
    } else if (request->type == I2C_OP_SCAN) {
        value = number_array_from_bytes(ctx, completion->data,
                                        completion->size);
    }
    if (JS_IsException(value)) {
        clear_exception(ctx);
        reject_request(state, request, "IO", "out of memory");
        return;
    }

    uint16_t slot = request->device_slot;
    uint32_t device_generation = request->device_generation;
    i2c_operation_t type = request->type;
    js_request_resolve(&state->requests, request->id, request->bus_generation, value);
    JS_FreeValue(ctx, value);

    if (type == I2C_OP_DEVICE_CLOSE &&
        slot < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS) {
        i2c_device_state_t *device = &state->devices[slot];
        if (device->generation == device_generation) {
            if (!JS_IsUndefined(device->close_promise)) {
                JS_FreeValue(ctx, device->close_promise);
                device->close_promise = JS_UNDEFINED;
            }
            device->active = false;
            device->closing = false;
            device->handle = NULL;
            device->close_request_id = 0;
        }
    }
}

static void i2c_event_handler(js_runtime_t *runtime,
                              const js_event_t *event, void *opaque)
{
    (void)runtime;
    js_i2c_module_t *module = opaque;
    if (event->source >= I2C_NUM_MAX) return;
    i2c_bus_state_t *state = &module->buses[event->source];
    if (!state->completions) return;
    i2c_completion_t completion;
    while (state->completions &&
           xQueueReceive(state->completions, &completion, 0) == pdTRUE) {
        bool bus_close = completion.type == I2C_OP_BUS_CLOSE;
        complete_request(state, &completion);
        free(completion.data);
        if (bus_close) {
            finalize_bus_state(state);
            break;
        }
    }
}

static void reject_all_requests(i2c_bus_state_t *state, const char *message)
{
    while (state->requests.head) {
        reject_request(state, state->requests.head->opaque, "CLOSED", message);
    }
}

static void force_close_bus(i2c_bus_state_t *state)
{
    if (!state->open) return;
    set_bus_closing(state, true);
    if (state->task) {
        xTaskNotifyGive(state->task);
        if (state->stopped) xSemaphoreTake(state->stopped, portMAX_DELAY);
    } else {
        close_native_bus(state);
    }
    state->task = NULL;
    reject_all_requests(state, "I2C closed");
    i2c_completion_t completion;
    while (state->completions &&
           xQueueReceive(state->completions, &completion, 0) == pdTRUE) {
        free(completion.data);
    }
    i2c_command_t command;
    while (state->commands &&
           xQueueReceive(state->commands, &command, 0) == pdTRUE) {
        free(command.write_data);
        free(command.read_data);
    }
    finalize_bus_state(state);
}

static JSValue new_request(JSContext *ctx, i2c_bus_state_t *state,
                           i2c_operation_t type, uint16_t device_slot,
                           uint32_t device_generation, uint16_t address,
                           uint32_t *out_id)
{
    i2c_request_t *request = calloc(1, sizeof(*request));
    if (!request) return JS_ThrowOutOfMemory(ctx);
    *request = (i2c_request_t){.bus_generation = state->generation,
        .device_generation = device_generation, .device_slot = device_slot,
        .address = address, .type = type};
    JSValue promise = js_request_create(&state->requests, state->generation,
                                        request, free, NULL, &request->id);
    if (JS_IsException(promise)) { free(request); return promise; }
    *out_id = request->id;
    return promise;
}

static JSValue enqueue_command(JSContext *ctx, i2c_bus_state_t *state,
                               i2c_command_t *command)
{
    if (state->requests.count >= CONFIG_JS_I2C_MAX_PENDING) {
        free(command->write_data);
        free(command->read_data);
        command->write_data = NULL;
        command->read_data = NULL;
        return rejected_promise(ctx, "QUEUE_FULL",
            "I2C operation queue full", command->type, state->id,
            command->address);
    }
    JSValue promise = new_request(ctx, state, command->type,
                                  command->device_slot,
                                  command->device_generation,
                                  command->address, &command->id);
    if (JS_IsException(promise)) {
        free(command->write_data);
        free(command->read_data);
        command->write_data = NULL;
        command->read_data = NULL;
        return promise;
    }
    command->bus_generation = state->generation;
    if ((command->type == I2C_OP_READ || command->type == I2C_OP_WRITE_READ) &&
        command->timeout_ms > 0) {
        command->deadline_us = esp_timer_get_time() + (int64_t)command->timeout_ms * 1000;
    }
    if (xQueueSend(state->commands, command, 0) != pdTRUE) {
        i2c_request_t *request = find_request(state, command->id,
                                               state->generation);
        if (request) reject_request(state, request, "QUEUE_FULL",
                                    "I2C operation queue full");
        free(command->write_data);
        free(command->read_data);
        command->write_data = NULL;
        command->read_data = NULL;
    }
    return promise;
}

static bool get_bus_data(JSContext *ctx, JSValueConst *func_data,
                         uint32_t *id, uint32_t *generation)
{
    return JS_ToUint32(ctx, id, func_data[0]) == 0 &&
           JS_ToUint32(ctx, generation, func_data[1]) == 0;
}

static bool decode_bus(JSContext *ctx, JSValueConst *func_data,
                       i2c_bus_state_t **out, i2c_operation_t operation)
{
    uint32_t id = 0;
    uint32_t generation = 0;
    if (!get_bus_data(ctx, func_data, &id, &generation)) return false;
    if (id >= I2C_NUM_MAX) {
        JS_ThrowInternalError(ctx, "invalid I2C bus handle");
        return false;
    }
    i2c_bus_state_t *state = &s_i2c.buses[id];
    if (!state->open || state->generation != generation ||
        bus_is_closing(state)) {
        throw_i2c_error(ctx, "CLOSED", "I2C bus closed", operation,
                        (int)id, -1);
        return false;
    }
    *out = state;
    return true;
}

static bool get_device_data(JSContext *ctx, JSValueConst *func_data,
                            uint32_t *bus_id, uint32_t *bus_generation,
                            uint32_t *slot, uint32_t *device_generation)
{
    return JS_ToUint32(ctx, bus_id, func_data[0]) == 0 &&
           JS_ToUint32(ctx, bus_generation, func_data[1]) == 0 &&
           JS_ToUint32(ctx, slot, func_data[2]) == 0 &&
           JS_ToUint32(ctx, device_generation, func_data[3]) == 0;
}

static bool decode_device(JSContext *ctx, JSValueConst *func_data,
                          i2c_bus_state_t **out_bus,
                          i2c_device_state_t **out_device,
                          i2c_operation_t operation)
{
    uint32_t bus_id = 0, bus_generation = 0, slot = 0;
    uint32_t device_generation = 0;
    if (!get_device_data(ctx, func_data, &bus_id, &bus_generation, &slot,
                         &device_generation)) return false;
    if (bus_id >= I2C_NUM_MAX ||
        slot >= CONFIG_JS_I2C_MAX_DEVICES_PER_BUS) {
        JS_ThrowInternalError(ctx, "invalid I2C device handle");
        return false;
    }
    i2c_bus_state_t *bus = &s_i2c.buses[bus_id];
    i2c_device_state_t *device = &bus->devices[slot];
    if (!bus->open || bus->generation != bus_generation ||
        bus_is_closing(bus) || !device->active || device->closing ||
        device->generation != device_generation) {
        throw_i2c_error(ctx, "CLOSED", "I2C device closed", operation,
                        (int)bus_id, device->address);
        return false;
    }
    *out_bus = bus;
    *out_device = device;
    return true;
}

static JSValue js_bus_closed(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv, int magic,
                             JSValue *func_data)
{
    (void)this_value; (void)argc; (void)argv; (void)magic;
    uint32_t id = 0, generation = 0;
    if (!get_bus_data(ctx, func_data, &id, &generation)) return JS_EXCEPTION;
    bool closed = id >= I2C_NUM_MAX || !s_i2c.buses[id].open ||
                  s_i2c.buses[id].generation != generation ||
                  bus_is_closing(&s_i2c.buses[id]);
    return JS_NewBool(ctx, closed);
}

static JSValue js_device_closed(JSContext *ctx, JSValueConst this_value,
                                int argc, JSValueConst *argv, int magic,
                                JSValue *func_data)
{
    (void)this_value; (void)argc; (void)argv; (void)magic;
    uint32_t bus_id = 0, bus_generation = 0, slot = 0;
    uint32_t device_generation = 0;
    if (!get_device_data(ctx, func_data, &bus_id, &bus_generation, &slot,
                         &device_generation)) return JS_EXCEPTION;
    bool closed = true;
    if (bus_id < I2C_NUM_MAX &&
        slot < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS) {
        i2c_bus_state_t *bus = &s_i2c.buses[bus_id];
        i2c_device_state_t *device = &bus->devices[slot];
        closed = !bus->open || bus->generation != bus_generation ||
                 bus_is_closing(bus) || !device->active || device->closing ||
                 device->generation != device_generation;
    }
    return JS_NewBool(ctx, closed);
}

static JSValue js_device_write(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *func_data)
{
    (void)this_value; (void)magic;
    i2c_bus_state_t *bus = NULL;
    i2c_device_state_t *device = NULL;
    if (!decode_device(ctx, func_data, &bus, &device, I2C_OP_WRITE)) {
        return JS_EXCEPTION;
    }
    if (argc < 1) return JS_ThrowTypeError(ctx, "data is required");
    i2c_command_t command = {
        .type = I2C_OP_WRITE,
        .device = device->handle,
        .address = device->address,
        .device_slot = (uint16_t)(device - bus->devices),
        .device_generation = device->generation,
    };
    if (!copy_data(ctx, argv[0], &command.write_data, &command.write_size) ||
        !parse_operation_options(ctx, argc, argv, 1,
                                 I2C_TIMEOUT_DEFAULT_MS,
                                 &command.timeout_ms)) {
        free(command.write_data);
        return JS_EXCEPTION;
    }
    return enqueue_command(ctx, bus, &command);
}

static JSValue js_device_read(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv, int magic,
                              JSValue *func_data)
{
    (void)this_value; (void)magic;
    i2c_bus_state_t *bus = NULL;
    i2c_device_state_t *device = NULL;
    if (!decode_device(ctx, func_data, &bus, &device, I2C_OP_READ)) {
        return JS_EXCEPTION;
    }
    int32_t length = 0;
    if (argc < 1 || !get_integer(ctx, argv[0], "length", 1,
                                 CONFIG_JS_I2C_MAX_TRANSFER_SIZE, &length)) {
        return JS_EXCEPTION;
    }
    i2c_command_t command = {
        .type = I2C_OP_READ,
        .device = device->handle,
        .address = device->address,
        .device_slot = (uint16_t)(device - bus->devices),
        .device_generation = device->generation,
        .read_size = (size_t)length,
    };
    if (!parse_operation_options(ctx, argc, argv, 1,
                                 I2C_TIMEOUT_DEFAULT_MS,
                                 &command.timeout_ms)) return JS_EXCEPTION;
    command.read_data = malloc(command.read_size);
    if (!command.read_data) return JS_ThrowOutOfMemory(ctx);
    return enqueue_command(ctx, bus, &command);
}

static JSValue js_device_write_read(JSContext *ctx,
                                    JSValueConst this_value, int argc,
                                    JSValueConst *argv, int magic,
                                    JSValue *func_data)
{
    (void)this_value; (void)magic;
    i2c_bus_state_t *bus = NULL;
    i2c_device_state_t *device = NULL;
    if (!decode_device(ctx, func_data, &bus, &device,
                       I2C_OP_WRITE_READ)) return JS_EXCEPTION;
    if (argc < 2) {
        return JS_ThrowTypeError(ctx, "data and readLength are required");
    }
    int32_t read_length = 0;
    i2c_command_t command = {
        .type = I2C_OP_WRITE_READ,
        .device = device->handle,
        .address = device->address,
        .device_slot = (uint16_t)(device - bus->devices),
        .device_generation = device->generation,
    };
    if (!copy_data(ctx, argv[0], &command.write_data, &command.write_size) ||
        !get_integer(ctx, argv[1], "readLength", 1,
                     CONFIG_JS_I2C_MAX_TRANSFER_SIZE, &read_length) ||
        !parse_operation_options(ctx, argc, argv, 2,
                                 I2C_TIMEOUT_DEFAULT_MS,
                                 &command.timeout_ms)) {
        free(command.write_data);
        return JS_EXCEPTION;
    }
    command.read_size = (size_t)read_length;
    command.read_data = malloc(command.read_size);
    if (!command.read_data) {
        free(command.write_data);
        return JS_ThrowOutOfMemory(ctx);
    }
    return enqueue_command(ctx, bus, &command);
}

static JSValue js_device_close(JSContext *ctx, JSValueConst this_value,
                               int argc, JSValueConst *argv, int magic,
                               JSValue *func_data)
{
    (void)this_value; (void)argc; (void)argv; (void)magic;
    uint32_t bus_id = 0, bus_generation = 0, slot = 0;
    uint32_t device_generation = 0;
    if (!get_device_data(ctx, func_data, &bus_id, &bus_generation, &slot,
                         &device_generation)) return JS_EXCEPTION;
    if (bus_id >= I2C_NUM_MAX ||
        slot >= CONFIG_JS_I2C_MAX_DEVICES_PER_BUS) return resolved_promise(ctx);
    i2c_bus_state_t *bus = &s_i2c.buses[bus_id];
    i2c_device_state_t *device = &bus->devices[slot];
    if (!bus->open || bus->generation != bus_generation ||
        !device->active || device->generation != device_generation) {
        return resolved_promise(ctx);
    }
    if (device->closing) return JS_DupValue(ctx, device->close_promise);
    if (bus_is_closing(bus)) return JS_DupValue(ctx, bus->close_promise);

    i2c_command_t command = {
        .type = I2C_OP_DEVICE_CLOSE,
        .device = device->handle,
        .address = device->address,
        .device_slot = (uint16_t)slot,
        .device_generation = device->generation,
        .bus_generation = bus->generation,
    };
    JSValue promise = new_request(ctx, bus, I2C_OP_DEVICE_CLOSE,
                                  (uint16_t)slot, device->generation,
                                  device->address, &command.id);
    if (JS_IsException(promise)) return promise;
    if (xQueueSend(bus->commands, &command, 0) != pdTRUE) {
        i2c_request_t *request = find_request(bus, command.id,
                                               bus->generation);
        if (request) reject_request(bus, request, "QUEUE_FULL",
                                    "I2C operation queue full");
        return promise;
    }
    device->closing = true;
    device->close_request_id = command.id;
    device->close_promise = JS_DupValue(ctx, promise);
    return promise;
}

static JSValue js_bus_probe(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv, int magic,
                            JSValue *func_data)
{
    (void)this_value; (void)magic;
    i2c_bus_state_t *bus = NULL;
    if (!decode_bus(ctx, func_data, &bus, I2C_OP_PROBE)) return JS_EXCEPTION;
    int32_t address = 0;
    if (argc < 1 || !get_integer(ctx, argv[0], "address", 0, 0x7f,
                                 &address)) return JS_EXCEPTION;
    i2c_command_t command = {
        .type = I2C_OP_PROBE,
        .address = (uint16_t)address,
    };
    if (!parse_operation_options(ctx, argc, argv, 1,
                                 I2C_PROBE_TIMEOUT_DEFAULT_MS,
                                 &command.timeout_ms)) return JS_EXCEPTION;
    return enqueue_command(ctx, bus, &command);
}

static JSValue js_bus_scan(JSContext *ctx, JSValueConst this_value,
                           int argc, JSValueConst *argv, int magic,
                           JSValue *func_data)
{
    (void)this_value; (void)magic;
    i2c_bus_state_t *bus = NULL;
    if (!decode_bus(ctx, func_data, &bus, I2C_OP_SCAN)) return JS_EXCEPTION;
    int32_t from = 0x08;
    int32_t to = 0x77;
    int32_t timeout = I2C_PROBE_TIMEOUT_DEFAULT_MS;
    if (argc > 0 && !JS_IsUndefined(argv[0])) {
        static const char *const fields[] = {"from", "to", "timeoutMs"};
        if (!options_object(ctx, argv[0], "scan") ||
            !validate_fields(ctx, argv[0], fields, 3, "scan")) {
            return JS_EXCEPTION;
        }
        bool present = false;
        if (!get_optional_integer(ctx, argv[0], "from", 0, 0x7f,
                                  &from, &present) ||
            !get_optional_integer(ctx, argv[0], "to", 0, 0x7f,
                                  &to, &present) ||
            !get_optional_integer(ctx, argv[0], "timeoutMs", 0,
                                  I2C_TIMEOUT_MAX_MS, &timeout, &present)) {
            return JS_EXCEPTION;
        }
    }
    if (from > to) return JS_ThrowRangeError(ctx, "from must not exceed to");
    i2c_command_t command = {
        .type = I2C_OP_SCAN,
        .address = UINT16_MAX,
        .scan_from = (uint16_t)from,
        .scan_to = (uint16_t)to,
        .timeout_ms = (uint32_t)timeout,
        .read_size = (size_t)(to - from + 1),
    };
    command.read_data = malloc(command.read_size);
    if (!command.read_data) return JS_ThrowOutOfMemory(ctx);
    return enqueue_command(ctx, bus, &command);
}

static JSValue js_bus_close(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv, int magic,
                            JSValue *func_data)
{
    (void)this_value; (void)argc; (void)argv; (void)magic;
    uint32_t id = 0, generation = 0;
    if (!get_bus_data(ctx, func_data, &id, &generation)) return JS_EXCEPTION;
    if (id >= I2C_NUM_MAX) return resolved_promise(ctx);
    i2c_bus_state_t *state = &s_i2c.buses[id];
    if (!state->open || state->generation != generation) {
        return resolved_promise(ctx);
    }
    if (bus_is_closing(state)) return JS_DupValue(ctx, state->close_promise);
    uint32_t request_id = 0;
    JSValue promise = new_request(ctx, state, I2C_OP_BUS_CLOSE, UINT16_MAX,
                                  0, UINT16_MAX, &request_id);
    if (JS_IsException(promise)) return promise;
    state->close_request_id = request_id;
    state->close_promise = JS_DupValue(ctx, promise);
    set_bus_closing(state, true);
    if (state->task) xTaskNotifyGive(state->task);
    return promise;
}

static bool add_data_function(JSContext *ctx, JSValue object,
                              const char *name, JSCFunctionData *function,
                              int argc, int data_len, JSValue *data)
{
    JSValue method = JS_NewCFunctionData(ctx, function, argc, 0,
                                         data_len, data);
    if (JS_IsException(method)) return false;
    return JS_SetPropertyStr(ctx, object, name, method) >= 0;
}

static bool add_data_getter(JSContext *ctx, JSValue object,
                            const char *name, JSCFunctionData *function,
                            int data_len, JSValue *data)
{
    JSValue getter = JS_NewCFunctionData(ctx, function, 0, 0,
                                         data_len, data);
    if (JS_IsException(getter)) return false;
    JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        JS_FreeValue(ctx, getter);
        return false;
    }
    int result = JS_DefinePropertyGetSet(ctx, object, atom, getter,
                                         JS_UNDEFINED, JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    return result >= 0;
}

static JSValue make_device_object(JSContext *ctx, i2c_bus_state_t *bus,
                                  uint32_t slot)
{
    i2c_device_state_t *device = &bus->devices[slot];
    JSValue data[4] = {
        JS_NewUint32(ctx, (uint32_t)bus->id),
        JS_NewUint32(ctx, bus->generation),
        JS_NewUint32(ctx, slot),
        JS_NewUint32(ctx, device->generation),
    };
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object) ||
        !set_readonly_property(ctx, object, "address",
                               JS_NewInt32(ctx, device->address)) ||
        !set_readonly_property(ctx, object, "addressBits",
                               JS_NewInt32(ctx, device->address_bits)) ||
        !set_readonly_property(ctx, object, "clockHz",
                               JS_NewInt32(ctx, (int32_t)device->clock_hz)) ||
        !add_data_getter(ctx, object, "closed", js_device_closed, 4, data) ||
        !add_data_function(ctx, object, "write", js_device_write, 2, 4, data) ||
        !add_data_function(ctx, object, "read", js_device_read, 2, 4, data) ||
        !add_data_function(ctx, object, "writeRead", js_device_write_read,
                           3, 4, data) ||
        !add_data_function(ctx, object, "close", js_device_close, 0, 4,
                           data)) {
        JS_FreeValue(ctx, object);
        object = JS_EXCEPTION;
    }
    for (size_t i = 0; i < 4; ++i) JS_FreeValue(ctx, data[i]);
    return object;
}

static JSValue js_bus_device(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv, int magic,
                             JSValue *func_data)
{
    (void)this_value; (void)magic;
    i2c_bus_state_t *bus = NULL;
    if (!decode_bus(ctx, func_data, &bus, I2C_OP_DEVICE)) return JS_EXCEPTION;
    i2c_device_options_t options;
    if (!parse_device_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED,
                              &options)) return JS_EXCEPTION;
    int32_t address = 0;
    int32_t max_address = options.address_bits == 7 ? 0x7f : 0x3ff;
    if (argc < 1 || !get_integer(ctx, argv[0], "address", 0, max_address,
                                 &address)) return JS_EXCEPTION;
    int free_slot = -1;
    for (size_t i = 0; i < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS; ++i) {
        i2c_device_state_t *device = &bus->devices[i];
        if (!device->active && free_slot < 0) free_slot = (int)i;
        if (device->active && device->address == address &&
            device->address_bits == options.address_bits) {
            return throw_i2c_error(ctx, "RESOURCE_BUSY",
                "I2C device address already open", I2C_OP_DEVICE,
                bus->id, address);
        }
    }
    if (free_slot < 0) {
        return JS_ThrowRangeError(ctx, "maximum I2C devices reached");
    }
    i2c_device_config_t config = {
        .dev_addr_length = options.address_bits == 7
            ? I2C_ADDR_BIT_LEN_7 : I2C_ADDR_BIT_LEN_10,
        .device_address = (uint16_t)address,
        .scl_speed_hz = options.clock_hz,
        .scl_wait_us = options.clock_stretch_timeout_us,
    };
    i2c_master_dev_handle_t handle = NULL;
    esp_err_t err = i2c_master_bus_add_device(bus->handle, &config, &handle);
    if (err != ESP_OK) {
        return throw_i2c_error(ctx, "IO", esp_err_to_name(err),
                               I2C_OP_DEVICE, bus->id, address);
    }
    i2c_device_state_t *device = &bus->devices[free_slot];
    device->generation++;
    if (device->generation == 0) device->generation = 1;
    device->handle = handle;
    device->address = (uint16_t)address;
    device->address_bits = options.address_bits;
    device->clock_hz = options.clock_hz;
    device->active = true;
    device->closing = false;
    device->close_request_id = 0;
    device->close_promise = JS_UNDEFINED;
    JSValue object = make_device_object(ctx, bus, (uint32_t)free_slot);
    if (JS_IsException(object)) {
        i2c_master_bus_rm_device(handle);
        device->handle = NULL;
        device->active = false;
    }
    return object;
}

static JSValue make_bus_object(JSContext *ctx, i2c_bus_state_t *state)
{
    JSValue data[2] = {
        JS_NewUint32(ctx, (uint32_t)state->id),
        JS_NewUint32(ctx, state->generation),
    };
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object) ||
        !set_readonly_property(ctx, object, "id",
                               JS_NewInt32(ctx, state->id)) ||
        !set_readonly_property(ctx, object, "sda",
                               JS_NewInt32(ctx, state->sda)) ||
        !set_readonly_property(ctx, object, "scl",
                               JS_NewInt32(ctx, state->scl)) ||
        !add_data_getter(ctx, object, "closed", js_bus_closed, 2, data) ||
        !add_data_function(ctx, object, "device", js_bus_device, 2, 2, data) ||
        !add_data_function(ctx, object, "probe", js_bus_probe, 2, 2, data) ||
        !add_data_function(ctx, object, "scan", js_bus_scan, 1, 2, data) ||
        !add_data_function(ctx, object, "close", js_bus_close, 0, 2, data)) {
        JS_FreeValue(ctx, object);
        object = JS_EXCEPTION;
    }
    JS_FreeValue(ctx, data[0]);
    JS_FreeValue(ctx, data[1]);
    return object;
}

static JSValue js_i2c_list(JSContext *ctx, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void)this_value; (void)argc; (void)argv;
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    for (int id = 0; id < I2C_NUM_MAX; ++id) {
        i2c_bus_state_t *state = &s_i2c.buses[id];
        bool allowed = port_allowed(&s_i2c, id);
        bool available = allowed && !state->open;
        JSValue item = JS_NewObject(ctx);
        if (JS_IsException(item) ||
            !set_property(ctx, item, "id", JS_NewInt32(ctx, id)) ||
            !set_property(ctx, item, "available", JS_NewBool(ctx, available)) ||
            !set_property(ctx, item, "open", JS_NewBool(ctx, state->open))) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
        if (!available) {
            const char *reason = !allowed ? "not-allowed" : "reserved";
            if (!set_property(ctx, item, "reason", JS_NewString(ctx, reason))) {
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

static JSValue js_i2c_open(JSContext *ctx, JSValueConst this_value,
                           int argc, JSValueConst *argv)
{
    (void)this_value;
    int32_t id = 0;
    if (argc < 1 || !get_integer(ctx, argv[0], "I2C id", 0,
                                 I2C_NUM_MAX - 1, &id)) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "I2C options are required");
    i2c_open_options_t options;
    if (!parse_open_options(ctx, argv[1], &options)) return JS_EXCEPTION;
    if (!port_allowed(&s_i2c, id)) {
        return JS_ThrowRangeError(ctx, "I2C controller is not allowed");
    }
    i2c_bus_state_t *state = &s_i2c.buses[id];
    if (state->open) {
        return throw_i2c_error(ctx, "RESOURCE_BUSY",
            "I2C controller already open", I2C_OP_OPEN, id, -1);
    }
    uint64_t pin_mask = BIT64(options.sda) | BIT64(options.scl);
    if (!pin_allowed(&s_i2c, options.sda) ||
        !pin_allowed(&s_i2c, options.scl)) {
        return JS_ThrowRangeError(ctx, "I2C pin is not allowed");
    }
    if (esp_gpio_is_reserved(pin_mask)) {
        return throw_i2c_error(ctx, "RESOURCE_BUSY", "I2C pin is reserved",
                               I2C_OP_OPEN, id, -1);
    }
    i2c_master_bus_config_t config = {
        .i2c_port = id,
        .sda_io_num = options.sda,
        .scl_io_num = options.scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = options.glitch_ignore_cycles,
        .flags.enable_internal_pullup = options.internal_pullups,
    };
    esp_err_t err = i2c_new_master_bus(&config, &state->handle);
    if (err != ESP_OK) {
        const char *code = err == ESP_ERR_NOT_FOUND
            ? "RESOURCE_BUSY" : "IO";
        return throw_i2c_error(ctx, code, esp_err_to_name(err),
                               I2C_OP_OPEN, id, -1);
    }
    uint64_t already_reserved = esp_gpio_reserve(pin_mask);
    state->manually_reserved_pin_mask = pin_mask & ~already_reserved;
    state->commands = xQueueCreate(CONFIG_JS_I2C_MAX_PENDING + 1,
                                    sizeof(i2c_command_t));
    state->completions = xQueueCreate(CONFIG_JS_I2C_MAX_PENDING + 2,
                                      sizeof(i2c_completion_t));
    state->stopped = xSemaphoreCreateBinary();
    if (!state->commands || !state->completions || !state->stopped) {
        state->open = true;
        force_close_bus(state);
        return JS_ThrowOutOfMemory(ctx);
    }
    state->generation++;
    if (state->generation == 0) state->generation = 1;
    state->sda = options.sda;
    state->scl = options.scl;
    state->open = true;
    state->closing = false;
    state->close_request_id = 0;
    state->close_promise = JS_UNDEFINED;
    BaseType_t created = xTaskCreate(i2c_worker, "js_i2c",
        CONFIG_JS_I2C_TASK_STACK_SIZE, state, CONFIG_JS_I2C_TASK_PRIORITY,
        &state->task);
    if (created != pdPASS) {
        force_close_bus(state);
        return JS_ThrowOutOfMemory(ctx);
    }
    JSValue object = make_bus_object(ctx, state);
    if (JS_IsException(object)) force_close_bus(state);
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

static esp_err_t install_error_constructor(JSContext *ctx, JSValue global,
                                           js_i2c_module_t *module)
{
    static const char source[] =
        "(class I2CError extends Error {"
        "constructor(code,message,operation,busId,address){"
        "super(message);this.name='I2CError';this.code=code;"
        "this.operation=operation;this.busId=busId;"
        "if(address!==undefined)this.address=address;"
        "}})";
    JSValue constructor = JS_Eval(ctx, source, sizeof(source) - 1,
                                  "<i2c>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(constructor)) return ESP_FAIL;
    module->error_constructor = JS_DupValue(ctx, constructor);
    if (JS_SetPropertyStr(ctx, global, "I2CError", constructor) < 0) {
        JS_FreeValue(ctx, module->error_constructor);
        module->error_constructor = JS_UNDEFINED;
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t i2c_module_init(js_runtime_t *runtime, JSContext *ctx,
                                 void *opaque)
{
    js_i2c_module_t *module = opaque;
    module->runtime = runtime;
    module->ctx = ctx;
    module->error_constructor = JS_UNDEFINED;
    for (int id = 0; id < I2C_NUM_MAX; ++id) {
        i2c_bus_state_t *state = &module->buses[id];
        state->module = module;
        js_request_table_init(&state->requests, ctx);
        esp_err_t tracked = js_runtime_track_requests(runtime, &state->requests);
        if (tracked != ESP_OK) return tracked;
        state->id = id;
        state->sda = -1;
        state->scl = -1;
        state->close_promise = JS_UNDEFINED;
        state->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
        for (size_t i = 0; i < CONFIG_JS_I2C_MAX_DEVICES_PER_BUS; ++i) {
            state->devices[i].close_promise = JS_UNDEFINED;
        }
    }
    esp_err_t err = js_event_register_handler(runtime, JS_EVENT_I2C,
                                               i2c_event_handler, module);
    if (err != ESP_OK) return err;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue i2c = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(i2c)) {
        JS_FreeValue(ctx, i2c);
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_I2C);
        return ESP_ERR_NO_MEM;
    }
    if (install_error_constructor(ctx, global, module) != ESP_OK ||
        install_method(ctx, i2c, "list", js_i2c_list, 0) != ESP_OK ||
        install_method(ctx, i2c, "open", js_i2c_open, 2) != ESP_OK) {
        JS_FreeValue(ctx, i2c);
        JS_FreeValue(ctx, global);
        if (!JS_IsUndefined(module->error_constructor)) {
            JS_FreeValue(ctx, module->error_constructor);
            module->error_constructor = JS_UNDEFINED;
        }
        js_event_unregister_handler(runtime, JS_EVENT_I2C);
        return ESP_FAIL;
    }
    if (JS_SetPropertyStr(ctx, global, "i2c", i2c) < 0) {
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, module->error_constructor);
        module->error_constructor = JS_UNDEFINED;
        js_event_unregister_handler(runtime, JS_EVENT_I2C);
        return ESP_FAIL;
    }
    JS_FreeValue(ctx, global);
    module->initialized = true;
    return ESP_OK;
}

static void i2c_module_deinit(js_runtime_t *runtime, JSContext *ctx,
                              void *opaque)
{
    js_i2c_module_t *module = opaque;
    module->initialized = false;
    for (int id = 0; id < I2C_NUM_MAX; ++id) {
        if (module->buses[id].open) force_close_bus(&module->buses[id]);
    }
    js_event_unregister_handler(runtime, JS_EVENT_I2C);
    if (!JS_IsUndefined(module->error_constructor)) {
        JS_FreeValue(ctx, module->error_constructor);
        module->error_constructor = JS_UNDEFINED;
    }
    module->runtime = NULL;
    module->ctx = NULL;
}

esp_err_t js_i2c_register(const js_i2c_config_t *config)
{
    if (!config || config->allowed_port_mask == 0 ||
        config->allowed_pin_mask == 0) return ESP_ERR_INVALID_ARG;
    if (s_registered) return ESP_ERR_INVALID_STATE;
    s_i2c.allowed_port_mask = config->allowed_port_mask;
    s_i2c.allowed_pin_mask = config->allowed_pin_mask;
    esp_err_t err = js_runtime_register_module(i2c_module_init,
                                               i2c_module_deinit, &s_i2c);
    if (err == ESP_OK) s_registered = true;
    return err;
}

#else

esp_err_t js_i2c_register(const js_i2c_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
