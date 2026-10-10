#include "js_registers.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "js_event.h"
#include "js_runtime.h"
#include "quickjs.h"
#include "sdkconfig.h"

#if CONFIG_JS_REGISTERS_ENABLED

typedef struct register_watch {
    uint32_t id;
    app_register_address_t *addresses;
    size_t address_count;
    JSValue callback;
    struct register_watch *next;
} register_watch_t;

typedef struct {
    uint64_t revision;
    app_register_source_t source;
    size_t change_count;
    app_register_change_t changes[];
} register_event_payload_t;

typedef struct {
    app_registers_t *registers;
    js_runtime_t *runtime;
    JSContext *ctx;
    register_watch_t *watches;
    uint32_t next_watch_id;
    uint32_t native_subscription_id;
    size_t watch_count;
    bool initialized;
} js_registers_module_t;

static js_registers_module_t s_module;
static bool s_registered;

static void clear_exception(JSContext *ctx)
{
    JSValue exception = JS_GetException(ctx);
    JS_FreeValue(ctx, exception);
}

static JSValue throw_register_error(JSContext *ctx, esp_err_t err,
                                    const char *operation)
{
    if (err == APP_REGISTERS_ERR_READ_ONLY) {
        return JS_ThrowTypeError(ctx, "%s cannot write a read-only register",
                                 operation);
    }
    if (err == APP_REGISTERS_ERR_DUPLICATE) {
        return JS_ThrowTypeError(ctx, "%s contains a duplicate address",
                                 operation);
    }
    if (err == ESP_ERR_NO_MEM) return JS_ThrowOutOfMemory(ctx);
    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_INVALID_SIZE) {
        return JS_ThrowRangeError(ctx, "%s range is invalid", operation);
    }
    return JS_ThrowInternalError(ctx, "%s failed: %s", operation,
                                 esp_err_to_name(err));
}

static bool parse_address(JSContext *ctx, JSValueConst value,
                          app_register_address_t *out)
{
    if (!JS_IsString(value)) {
        JS_ThrowTypeError(ctx, "register address must be a string");
        return false;
    }
    const char *text = JS_ToCString(ctx, value);
    if (!text) return false;
    esp_err_t err = app_register_address_parse(text, out);
    JS_FreeCString(ctx, text);
    if (err != ESP_OK) {
        JS_ThrowRangeError(ctx, "invalid register address");
        return false;
    }
    return true;
}

static JSValue value_to_js(JSContext *ctx, app_register_address_t address,
                           uint16_t value)
{
    return app_register_area_is_bit(address.area)
        ? JS_NewBool(ctx, value != 0)
        : JS_NewInt32(ctx, value);
}

static bool parse_integer(JSContext *ctx, JSValueConst value,
                          uint32_t maximum, const char *name,
                          uint32_t *out)
{
    if (!JS_IsNumber(value)) {
        JS_ThrowTypeError(ctx, "%s must be an integer", name);
        return false;
    }
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) || !isfinite(number) ||
        floor(number) != number || number < 0 || number > maximum) {
        JS_ThrowRangeError(ctx, "%s must be between 0 and %lu", name,
                           (unsigned long)maximum);
        return false;
    }
    *out = (uint32_t)number;
    return true;
}

static bool parse_register_value(JSContext *ctx,
                                 app_register_address_t address,
                                 JSValueConst value, uint16_t *out)
{
    if (app_register_area_is_bit(address.area)) {
        if (JS_IsBool(value)) {
            int parsed = JS_ToBool(ctx, value);
            if (parsed < 0) return false;
            *out = parsed ? 1 : 0;
            return true;
        }
        uint32_t parsed = 0;
        if (!parse_integer(ctx, value, 1, "bit value", &parsed)) return false;
        *out = (uint16_t)parsed;
        return true;
    }
    uint32_t parsed = 0;
    if (!parse_integer(ctx, value, UINT16_MAX, "word value", &parsed)) {
        return false;
    }
    *out = (uint16_t)parsed;
    return true;
}

static bool array_like_length(JSContext *ctx, JSValueConst value,
                              size_t maximum, size_t *out_count)
{
    bool array = JS_IsArray(ctx, value);
    if (!array) {
        size_t offset = 0, byte_length = 0, element_size = 0;
        JSValue buffer = JS_GetTypedArrayBuffer(ctx, value, &offset,
                                                &byte_length, &element_size);
        if (JS_IsException(buffer)) {
            clear_exception(ctx);
            JS_ThrowTypeError(ctx, "value must be an Array or TypedArray");
            return false;
        }
        JS_FreeValue(ctx, buffer);
    }
    JSValue length_value = JS_GetPropertyStr(ctx, value, "length");
    if (JS_IsException(length_value)) return false;
    uint32_t length = 0;
    int result = JS_ToUint32(ctx, &length, length_value);
    JS_FreeValue(ctx, length_value);
    if (result) return false;
    if (length == 0 || length > maximum) {
        JS_ThrowRangeError(ctx, "array length must be between 1 and %u",
                           (unsigned)maximum);
        return false;
    }
    *out_count = length;
    return true;
}

static bool parse_address_array(JSContext *ctx, JSValueConst value,
                                size_t maximum,
                                app_register_address_t **out_addresses,
                                size_t *out_count)
{
    size_t count = 0;
    if (!array_like_length(ctx, value, maximum, &count)) return false;
    app_register_address_t *addresses = calloc(count, sizeof(*addresses));
    if (!addresses) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, value, (uint32_t)i);
        bool valid = !JS_IsException(item) &&
                     parse_address(ctx, item, &addresses[i]);
        JS_FreeValue(ctx, item);
        if (!valid) {
            free(addresses);
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (addresses[i].area == addresses[j].area &&
                addresses[i].index == addresses[j].index) {
                free(addresses);
                JS_ThrowTypeError(ctx, "duplicate register address");
                return false;
            }
        }
    }
    *out_addresses = addresses;
    *out_count = count;
    return true;
}

static JSValue uint8_array(JSContext *ctx, const uint8_t *values, size_t count)
{
    JSValue buffer = JS_NewArrayBufferCopy(ctx, values, count);
    if (JS_IsException(buffer)) return buffer;
    JSValue result = JS_NewTypedArray(ctx, 1, &buffer,
                                      JS_TYPED_ARRAY_UINT8);
    JS_FreeValue(ctx, buffer);
    return result;
}

static JSValue uint16_array(JSContext *ctx, const uint16_t *values,
                            size_t count)
{
    JSValue buffer = JS_NewArrayBufferCopy(ctx, (const uint8_t *)values,
                                           count * sizeof(*values));
    if (JS_IsException(buffer)) return buffer;
    JSValue result = JS_NewTypedArray(ctx, 1, &buffer,
                                      JS_TYPED_ARRAY_UINT16);
    JS_FreeValue(ctx, buffer);
    return result;
}

static JSValue js_register_read(JSContext *ctx, JSValueConst this_value,
                                int argc, JSValueConst *argv)
{
    (void)this_value;
    app_register_address_t address;
    if (argc < 1 || !parse_address(ctx, argv[0], &address)) {
        return JS_EXCEPTION;
    }
    uint16_t value = 0;
    esp_err_t err = app_registers_read(s_module.registers, address, &value,
                                       NULL);
    if (err != ESP_OK) return throw_register_error(ctx, err, "registers.read");
    return value_to_js(ctx, address, value);
}

static JSValue js_register_write(JSContext *ctx, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void)this_value;
    app_register_address_t address;
    if (argc < 1 || !parse_address(ctx, argv[0], &address)) {
        return JS_EXCEPTION;
    }
    if (argc < 2) return JS_ThrowTypeError(ctx, "value is required");
    uint16_t value = 0;
    if (!parse_register_value(ctx, address, argv[1], &value)) {
        return JS_EXCEPTION;
    }
    uint64_t revision = 0;
    esp_err_t err = app_registers_write(s_module.registers, address, value,
                                        APP_REGISTER_SOURCE_JS, &revision);
    if (err != ESP_OK) {
        return throw_register_error(ctx, err, "registers.write");
    }
    return JS_NewFloat64(ctx, (double)revision);
}

static bool parse_count(JSContext *ctx, int argc, JSValueConst *argv,
                        uint32_t *out)
{
    if (argc < 2) {
        JS_ThrowTypeError(ctx, "count is required");
        return false;
    }
    if (!parse_integer(ctx, argv[1], CONFIG_APP_REGISTERS_MAX_TRANSACTION,
                       "count", out)) return false;
    if (*out == 0) {
        JS_ThrowRangeError(ctx, "count must be greater than zero");
        return false;
    }
    return true;
}

static JSValue js_register_read_bits(JSContext *ctx,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void)this_value;
    app_register_address_t start;
    uint32_t count = 0;
    if (argc < 1 || !parse_address(ctx, argv[0], &start) ||
        !parse_count(ctx, argc, argv, &count)) return JS_EXCEPTION;
    if (!app_register_area_is_bit(start.area)) {
        return JS_ThrowTypeError(ctx, "readBits requires an X, Y, or M address");
    }
    uint8_t *values = malloc(count);
    if (!values) return JS_ThrowOutOfMemory(ctx);
    esp_err_t err = app_registers_read_bits(s_module.registers, start, values,
                                            count, NULL);
    if (err != ESP_OK) {
        free(values);
        return throw_register_error(ctx, err, "registers.readBits");
    }
    JSValue result = uint8_array(ctx, values, count);
    free(values);
    return result;
}

static JSValue js_register_read_words(JSContext *ctx,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void)this_value;
    app_register_address_t start;
    uint32_t count = 0;
    if (argc < 1 || !parse_address(ctx, argv[0], &start) ||
        !parse_count(ctx, argc, argv, &count)) return JS_EXCEPTION;
    if (start.area != APP_REGISTER_AREA_D) {
        return JS_ThrowTypeError(ctx, "readWords requires a D address");
    }
    uint16_t *values = malloc(count * sizeof(*values));
    if (!values) return JS_ThrowOutOfMemory(ctx);
    esp_err_t err = app_registers_read_words(s_module.registers, start, values,
                                             count, NULL);
    if (err != ESP_OK) {
        free(values);
        return throw_register_error(ctx, err, "registers.readWords");
    }
    JSValue result = uint16_array(ctx, values, count);
    free(values);
    return result;
}

static bool parse_bit_values(JSContext *ctx, JSValueConst value,
                             uint8_t **out_values, size_t *out_count)
{
    size_t count = 0;
    if (!array_like_length(ctx, value, CONFIG_APP_REGISTERS_MAX_TRANSACTION,
                           &count)) return false;
    uint8_t *values = malloc(count);
    if (!values) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, value, (uint32_t)i);
        app_register_address_t bit_address = {.area = APP_REGISTER_AREA_M};
        uint16_t parsed = 0;
        bool valid = !JS_IsException(item) &&
                     parse_register_value(ctx, bit_address, item, &parsed);
        JS_FreeValue(ctx, item);
        if (!valid) {
            free(values);
            return false;
        }
        values[i] = (uint8_t)parsed;
    }
    *out_values = values;
    *out_count = count;
    return true;
}

static bool parse_word_values(JSContext *ctx, JSValueConst value,
                              uint16_t **out_values, size_t *out_count)
{
    size_t count = 0;
    if (!array_like_length(ctx, value, CONFIG_APP_REGISTERS_MAX_TRANSACTION,
                           &count)) return false;
    uint16_t *values = malloc(count * sizeof(*values));
    if (!values) {
        JS_ThrowOutOfMemory(ctx);
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        JSValue item = JS_GetPropertyUint32(ctx, value, (uint32_t)i);
        app_register_address_t word_address = {.area = APP_REGISTER_AREA_D};
        bool valid = !JS_IsException(item) &&
                     parse_register_value(ctx, word_address, item, &values[i]);
        JS_FreeValue(ctx, item);
        if (!valid) {
            free(values);
            return false;
        }
    }
    *out_values = values;
    *out_count = count;
    return true;
}

static JSValue js_register_write_bits(JSContext *ctx,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void)this_value;
    app_register_address_t start;
    if (argc < 1 || !parse_address(ctx, argv[0], &start)) return JS_EXCEPTION;
    if (!app_register_area_is_bit(start.area)) {
        return JS_ThrowTypeError(ctx, "writeBits requires an X, Y, or M address");
    }
    if (argc < 2) return JS_ThrowTypeError(ctx, "values are required");
    uint8_t *values = NULL;
    size_t count = 0;
    if (!parse_bit_values(ctx, argv[1], &values, &count)) return JS_EXCEPTION;
    uint64_t revision = 0;
    esp_err_t err = app_registers_write_bits(s_module.registers, start, values,
                                             count, APP_REGISTER_SOURCE_JS,
                                             &revision);
    free(values);
    if (err != ESP_OK) {
        return throw_register_error(ctx, err, "registers.writeBits");
    }
    return JS_NewFloat64(ctx, (double)revision);
}

static JSValue js_register_write_words(JSContext *ctx,
                                       JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    (void)this_value;
    app_register_address_t start;
    if (argc < 1 || !parse_address(ctx, argv[0], &start)) return JS_EXCEPTION;
    if (start.area != APP_REGISTER_AREA_D) {
        return JS_ThrowTypeError(ctx, "writeWords requires a D address");
    }
    if (argc < 2) return JS_ThrowTypeError(ctx, "values are required");
    uint16_t *values = NULL;
    size_t count = 0;
    if (!parse_word_values(ctx, argv[1], &values, &count)) return JS_EXCEPTION;
    uint64_t revision = 0;
    esp_err_t err = app_registers_write_words(s_module.registers, start, values,
                                              count, APP_REGISTER_SOURCE_JS,
                                              &revision);
    free(values);
    if (err != ESP_OK) {
        return throw_register_error(ctx, err, "registers.writeWords");
    }
    return JS_NewFloat64(ctx, (double)revision);
}

static JSValue js_register_read_many(JSContext *ctx,
                                     JSValueConst this_value,
                                     int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc < 1) return JS_ThrowTypeError(ctx, "addresses are required");
    app_register_address_t *addresses = NULL;
    size_t count = 0;
    if (!parse_address_array(ctx, argv[0],
                             CONFIG_APP_REGISTERS_MAX_TRANSACTION,
                             &addresses, &count)) return JS_EXCEPTION;
    uint16_t *values = malloc(count * sizeof(*values));
    if (!values) {
        free(addresses);
        return JS_ThrowOutOfMemory(ctx);
    }
    esp_err_t err = app_registers_read_many(s_module.registers, addresses,
                                            values, count, NULL);
    if (err != ESP_OK) {
        free(values);
        free(addresses);
        return throw_register_error(ctx, err, "registers.readMany");
    }
    JSValue object = JS_NewObject(ctx);
    char name[8];
    for (size_t i = 0; i < count && !JS_IsException(object); ++i) {
        if (app_register_address_format(addresses[i], name, sizeof(name)) !=
                ESP_OK ||
            JS_SetPropertyStr(ctx, object, name,
                value_to_js(ctx, addresses[i], values[i])) < 0) {
            JS_FreeValue(ctx, object);
            object = JS_EXCEPTION;
        }
    }
    free(values);
    free(addresses);
    return object;
}

static JSValue js_register_write_many(JSContext *ctx,
                                      JSValueConst this_value,
                                      int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc < 1 || !JS_IsObject(argv[0]) || JS_IsNull(argv[0]) ||
        JS_IsArray(ctx, argv[0]) || JS_IsFunction(ctx, argv[0])) {
        return JS_ThrowTypeError(ctx, "writes must be an object");
    }
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, argv[0],
                              JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
        return JS_EXCEPTION;
    }
    if (count == 0 || count > CONFIG_APP_REGISTERS_MAX_TRANSACTION) {
        JS_FreePropertyEnum(ctx, properties, count);
        return JS_ThrowRangeError(ctx, "invalid write count");
    }
    app_register_write_t *writes = calloc(count, sizeof(*writes));
    if (!writes) {
        JS_FreePropertyEnum(ctx, properties, count);
        return JS_ThrowOutOfMemory(ctx);
    }
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) {
            valid = false;
            break;
        }
        if (app_register_address_parse(name, &writes[i].address) != ESP_OK) {
            JS_ThrowRangeError(ctx, "invalid register address: %s", name);
            JS_FreeCString(ctx, name);
            valid = false;
            break;
        }
        JS_FreeCString(ctx, name);
        JSValue value = JS_GetProperty(ctx, argv[0], properties[i].atom);
        valid = !JS_IsException(value) &&
                parse_register_value(ctx, writes[i].address, value,
                                     &writes[i].value);
        JS_FreeValue(ctx, value);
        if (!valid) break;
    }
    JS_FreePropertyEnum(ctx, properties, count);
    if (!valid) {
        free(writes);
        return JS_EXCEPTION;
    }
    uint64_t revision = 0;
    esp_err_t err = app_registers_write_many(s_module.registers, writes,
                                             count, APP_REGISTER_SOURCE_JS,
                                             &revision);
    free(writes);
    if (err != ESP_OK) {
        return throw_register_error(ctx, err, "registers.writeMany");
    }
    return JS_NewFloat64(ctx, (double)revision);
}

static bool watch_matches(const register_watch_t *watch,
                          app_register_address_t address)
{
    for (size_t i = 0; i < watch->address_count; ++i) {
        if (watch->addresses[i].area == address.area &&
            watch->addresses[i].index == address.index) return true;
    }
    return false;
}

static JSValue make_watch_event(JSContext *ctx, const register_watch_t *watch,
                                const register_event_payload_t *payload)
{
    JSValue object = JS_NewObject(ctx);
    JSValue changes = JS_NewArray(ctx);
    if (JS_IsException(object) || JS_IsException(changes)) {
        JS_FreeValue(ctx, object);
        JS_FreeValue(ctx, changes);
        return JS_EXCEPTION;
    }
    uint32_t output_index = 0;
    char name[8];
    for (size_t i = 0; i < payload->change_count; ++i) {
        const app_register_change_t *change = &payload->changes[i];
        if (!watch_matches(watch, change->address)) continue;
        JSValue item = JS_NewObject(ctx);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, changes);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        if (app_register_address_format(change->address, name,
                                        sizeof(name)) != ESP_OK ||
            JS_SetPropertyStr(ctx, item, "address",
                              JS_NewString(ctx, name)) < 0 ||
            JS_SetPropertyStr(ctx, item, "oldValue",
                value_to_js(ctx, change->address, change->old_value)) < 0 ||
            JS_SetPropertyStr(ctx, item, "value",
                value_to_js(ctx, change->address, change->value)) < 0) {
            JS_FreeValue(ctx, item);
            JS_FreeValue(ctx, changes);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        if (JS_SetPropertyUint32(ctx, changes, output_index++, item) < 0) {
            JS_FreeValue(ctx, changes);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
    }
    if (JS_SetPropertyStr(ctx, object, "revision",
                          JS_NewFloat64(ctx, (double)payload->revision)) < 0 ||
        JS_SetPropertyStr(ctx, object, "source",
            JS_NewString(ctx, app_register_source_name(payload->source))) < 0 ||
        JS_SetPropertyStr(ctx, object, "changes", changes) < 0) {
        JS_FreeValue(ctx, object);
        return JS_EXCEPTION;
    }
    return object;
}

static void register_event_handler(js_runtime_t *runtime,
                                   const js_event_t *event, void *opaque)
{
    (void)event;
    js_registers_module_t *module = opaque;
    const register_event_payload_t *payload = event->payload;
    if (!payload) return;
    size_t count = 0;
    for (register_watch_t *watch = module->watches; watch; watch = watch->next) {
        for (size_t i = 0; i < payload->change_count; ++i) {
            if (watch_matches(watch, payload->changes[i].address)) {
                ++count;
                break;
            }
        }
    }
    if (count == 0) return;
    JSValue *callbacks = calloc(count, sizeof(*callbacks));
    JSValue *events = calloc(count, sizeof(*events));
    if (!callbacks || !events) {
        free(callbacks);
        free(events);
        return;
    }
    size_t index = 0;
    for (register_watch_t *watch = module->watches; watch; watch = watch->next) {
        bool matches = false;
        for (size_t i = 0; i < payload->change_count; ++i) {
            if (watch_matches(watch, payload->changes[i].address)) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;
        callbacks[index] = JS_DupValue(module->ctx, watch->callback);
        events[index] = make_watch_event(module->ctx, watch, payload);
        ++index;
    }
    for (size_t i = 0; i < count; ++i) {
        if (!JS_IsException(events[i])) {
            JSValueConst argv[] = {events[i]};
            js_runtime_call(runtime, callbacks[i], JS_UNDEFINED, 1, argv);
        } else {
            clear_exception(module->ctx);
        }
        JS_FreeValue(module->ctx, events[i]);
        JS_FreeValue(module->ctx, callbacks[i]);
    }
    free(events);
    free(callbacks);
}

static void native_register_event(const app_register_event_t *event,
                                  void *opaque)
{
    js_registers_module_t *module = opaque;
    if (!module->initialized || !module->runtime) return;
    size_t size = sizeof(register_event_payload_t) +
                  event->change_count * sizeof(app_register_change_t);
    register_event_payload_t *payload = malloc(size);
    if (!payload) return;
    payload->revision = event->revision;
    payload->source = event->source;
    payload->change_count = event->change_count;
    memcpy(payload->changes, event->changes,
           event->change_count * sizeof(app_register_change_t));
    js_event_t js_event = {
        .type = JS_EVENT_REGISTERS,
        .payload = payload,
        .payload_size = size,
        .free_payload = free,
    };
    js_event_post(module->runtime, &js_event, 0);
}

static bool remove_watch(js_registers_module_t *module, uint32_t id)
{
    register_watch_t **link = &module->watches;
    while (*link) {
        register_watch_t *watch = *link;
        if (watch->id == id) {
            *link = watch->next;
            JS_FreeValue(module->ctx, watch->callback);
            free(watch->addresses);
            free(watch);
            --module->watch_count;
            return true;
        }
        link = &watch->next;
    }
    return false;
}

static JSValue unsubscribe_function(JSContext *ctx,
                                    JSValueConst this_value, int argc,
                                    JSValueConst *argv, int magic,
                                    JSValue *func_data)
{
    (void)this_value; (void)argc; (void)argv; (void)magic;
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, func_data[0])) return JS_EXCEPTION;
    if (s_module.initialized) remove_watch(&s_module, id);
    return JS_UNDEFINED;
}

static JSValue js_register_watch(JSContext *ctx, JSValueConst this_value,
                                 int argc, JSValueConst *argv)
{
    (void)this_value;
    if (argc < 2 || !JS_IsFunction(ctx, argv[1])) {
        return JS_ThrowTypeError(ctx, "watch requires addresses and callback");
    }
    if (s_module.watch_count >= CONFIG_JS_REGISTERS_MAX_WATCHERS) {
        return JS_ThrowRangeError(ctx, "too many register watchers");
    }
    app_register_address_t *addresses = NULL;
    size_t address_count = 0;
    if (JS_IsString(argv[0])) {
        addresses = malloc(sizeof(*addresses));
        if (!addresses) return JS_ThrowOutOfMemory(ctx);
        if (!parse_address(ctx, argv[0], addresses)) {
            free(addresses);
            return JS_EXCEPTION;
        }
        address_count = 1;
    } else if (!parse_address_array(ctx, argv[0],
                    CONFIG_JS_REGISTERS_MAX_WATCH_ADDRESSES,
                    &addresses, &address_count)) {
        return JS_EXCEPTION;
    }
    register_watch_t *watch = calloc(1, sizeof(*watch));
    if (!watch) {
        free(addresses);
        return JS_ThrowOutOfMemory(ctx);
    }
    uint32_t id = ++s_module.next_watch_id;
    if (id == 0) id = ++s_module.next_watch_id;
    watch->id = id;
    watch->addresses = addresses;
    watch->address_count = address_count;
    watch->callback = JS_DupValue(ctx, argv[1]);
    register_watch_t **tail = &s_module.watches;
    while (*tail) tail = &(*tail)->next;
    *tail = watch;
    ++s_module.watch_count;

    JSValue data = JS_NewUint32(ctx, id);
    JSValue function = JS_NewCFunctionData(ctx, unsubscribe_function, 0, 0,
                                           1, &data);
    JS_FreeValue(ctx, data);
    if (JS_IsException(function)) remove_watch(&s_module, id);
    return function;
}

static JSValue js_register_status(JSContext *ctx, JSValueConst this_value,
                                  int argc, JSValueConst *argv)
{
    (void)this_value; (void)argc; (void)argv;
    JSValue status = JS_NewObject(ctx);
    JSValue capacity = JS_NewObject(ctx);
    if (JS_IsException(status) || JS_IsException(capacity)) {
        JS_FreeValue(ctx, status);
        JS_FreeValue(ctx, capacity);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(ctx, capacity, "X", JS_NewInt32(ctx,
            (int)app_registers_area_count(APP_REGISTER_AREA_X))) < 0 ||
        JS_SetPropertyStr(ctx, capacity, "Y", JS_NewInt32(ctx,
            (int)app_registers_area_count(APP_REGISTER_AREA_Y))) < 0 ||
        JS_SetPropertyStr(ctx, capacity, "M", JS_NewInt32(ctx,
            (int)app_registers_area_count(APP_REGISTER_AREA_M))) < 0 ||
        JS_SetPropertyStr(ctx, capacity, "D", JS_NewInt32(ctx,
            (int)app_registers_area_count(APP_REGISTER_AREA_D))) < 0 ||
        JS_SetPropertyStr(ctx, status, "revision", JS_NewFloat64(ctx,
            (double)app_registers_revision(s_module.registers))) < 0 ||
        JS_SetPropertyStr(ctx, status, "watcherCount",
                          JS_NewInt32(ctx, (int)s_module.watch_count)) < 0) {
        JS_FreeValue(ctx, capacity);
        JS_FreeValue(ctx, status);
        return JS_EXCEPTION;
    }
    if (JS_SetPropertyStr(ctx, status, "capacity", capacity) < 0) {
        JS_FreeValue(ctx, status);
        return JS_EXCEPTION;
    }
    return status;
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

static esp_err_t registers_module_init(js_runtime_t *runtime, JSContext *ctx,
                                       void *opaque)
{
    js_registers_module_t *module = opaque;
    module->runtime = runtime;
    module->ctx = ctx;
    module->watches = NULL;
    module->watch_count = 0;
    module->next_watch_id = 0;
    esp_err_t err = js_event_register_handler(runtime, JS_EVENT_REGISTERS,
                                               register_event_handler,
                                               module);
    if (err != ESP_OK) return err;
    err = app_registers_subscribe(module->registers, native_register_event,
                                  module, &module->native_subscription_id);
    if (err != ESP_OK) {
        js_event_unregister_handler(runtime, JS_EVENT_REGISTERS);
        return err;
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue registers = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(registers)) {
        JS_FreeValue(ctx, registers);
        JS_FreeValue(ctx, global);
        app_registers_unsubscribe(module->registers,
                                  module->native_subscription_id);
        js_event_unregister_handler(runtime, JS_EVENT_REGISTERS);
        return ESP_ERR_NO_MEM;
    }
    if (install_method(ctx, registers, "read", js_register_read, 1) != ESP_OK ||
        install_method(ctx, registers, "write", js_register_write, 2) != ESP_OK ||
        install_method(ctx, registers, "readBits", js_register_read_bits, 2) != ESP_OK ||
        install_method(ctx, registers, "writeBits", js_register_write_bits, 2) != ESP_OK ||
        install_method(ctx, registers, "readWords", js_register_read_words, 2) != ESP_OK ||
        install_method(ctx, registers, "writeWords", js_register_write_words, 2) != ESP_OK ||
        install_method(ctx, registers, "readMany", js_register_read_many, 1) != ESP_OK ||
        install_method(ctx, registers, "writeMany", js_register_write_many, 1) != ESP_OK ||
        install_method(ctx, registers, "watch", js_register_watch, 2) != ESP_OK ||
        install_method(ctx, registers, "status", js_register_status, 0) != ESP_OK) {
        JS_FreeValue(ctx, registers);
        JS_FreeValue(ctx, global);
        app_registers_unsubscribe(module->registers,
                                  module->native_subscription_id);
        js_event_unregister_handler(runtime, JS_EVENT_REGISTERS);
        return ESP_FAIL;
    }
    if (JS_SetPropertyStr(ctx, global, "registers", registers) < 0) {
        JS_FreeValue(ctx, global);
        app_registers_unsubscribe(module->registers,
                                  module->native_subscription_id);
        js_event_unregister_handler(runtime, JS_EVENT_REGISTERS);
        return ESP_FAIL;
    }
    JS_FreeValue(ctx, global);
    module->initialized = true;
    return ESP_OK;
}

static void registers_module_deinit(js_runtime_t *runtime, JSContext *ctx,
                                    void *opaque)
{
    js_registers_module_t *module = opaque;
    module->initialized = false;
    app_registers_unsubscribe(module->registers,
                              module->native_subscription_id);
    module->native_subscription_id = 0;
    while (module->watches) remove_watch(module, module->watches->id);
    js_event_unregister_handler(runtime, JS_EVENT_REGISTERS);
    module->runtime = NULL;
    module->ctx = NULL;
    (void)ctx;
}

esp_err_t js_registers_register(app_registers_t *registers)
{
    if (!registers) return ESP_ERR_INVALID_ARG;
    if (s_registered) return ESP_ERR_INVALID_STATE;
    s_module.registers = registers;
    esp_err_t err = js_runtime_register_module(registers_module_init,
                                               registers_module_deinit,
                                               &s_module);
    if (err == ESP_OK) s_registered = true;
    return err;
}

#else

esp_err_t js_registers_register(app_registers_t *registers)
{
    (void)registers;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
