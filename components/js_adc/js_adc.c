#include "js_adc.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "sdkconfig.h"
#include "js_runtime.h"
#include "quickjs.h"

#if CONFIG_JS_ADC_ENABLED
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_private/esp_gpio_reserve.h"

typedef struct {
    bool configured;
    adc_channel_t channel;
    adc_atten_t attenuation;
    adc_cali_handle_t calibration;
} adc_pin_t;

static struct {
    uint64_t allowed_pin_mask;
    adc_oneshot_unit_handle_t unit;
    unsigned configured_count;
    adc_pin_t pins[GPIO_NUM_MAX];
} s_adc;
static bool s_registered;

static JSValue throw_driver(JSContext *ctx, const char *operation, esp_err_t err)
{
    return JS_ThrowInternalError(ctx, "%s failed: %s", operation,
                                 esp_err_to_name(err));
}

static bool adc1_pin(int pin, adc_channel_t *channel)
{
    adc_unit_t unit;
    return pin >= 0 && pin < GPIO_NUM_MAX && pin < 64 &&
           adc_oneshot_io_to_channel(pin, &unit, channel) == ESP_OK &&
           unit == ADC_UNIT_1;
}

static bool get_pin(JSContext *ctx, int argc, JSValueConst *argv, int *pin)
{
    if (argc == 0 || !JS_IsNumber(argv[0])) {
        JS_ThrowTypeError(ctx, "pin must be an integer GPIO number");
        return false;
    }
    double value;
    if (JS_ToFloat64(ctx, &value, argv[0])) return false;
    if (!isfinite(value) || floor(value) != value) {
        JS_ThrowTypeError(ctx, "pin must be an integer GPIO number");
        return false;
    }
    adc_channel_t channel;
    if (value < 0 || value >= GPIO_NUM_MAX ||
        !adc1_pin((int)value, &channel)) {
        JS_ThrowRangeError(ctx, "pin must support ADC1");
        return false;
    }
    *pin = (int)value;
    return true;
}

static bool parse_options(JSContext *ctx, JSValueConst options,
                           adc_atten_t *attenuation)
{
    *attenuation = ADC_ATTEN_DB_12;
    if (JS_IsUndefined(options)) return true;
    if (!JS_IsObject(options) || JS_IsArray(ctx, options) ||
        JS_IsFunction(ctx, options)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return false;
    }
    JSPropertyEnum *properties = NULL;
    uint32_t count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &count, options,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
        return false;
    }
    bool valid = true;
    for (uint32_t i = 0; i < count; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) { valid = false; break; }
        if (strcmp(name, "attenuation") != 0) {
            JS_ThrowTypeError(ctx, "unknown ADC option: %s", name);
            valid = false;
        }
        JS_FreeCString(ctx, name);
        if (!valid) break;
    }
    JS_FreePropertyEnum(ctx, properties, count);
    if (!valid) return false;
    JSValue property = JS_GetPropertyStr(ctx, options, "attenuation");
    if (JS_IsException(property)) return false;
    if (JS_IsUndefined(property)) return true;
    double value = 0;
    if (!JS_IsNumber(property)) {
        JS_FreeValue(ctx, property);
        JS_ThrowTypeError(ctx, "attenuation must be a number in dB");
        return false;
    }
    int result = JS_ToFloat64(ctx, &value, property);
    JS_FreeValue(ctx, property);
    if (result) return false;
    if (value == 0) *attenuation = ADC_ATTEN_DB_0;
    else if (value == 2.5) *attenuation = ADC_ATTEN_DB_2_5;
    else if (value == 6) *attenuation = ADC_ATTEN_DB_6;
    else if (value == 12) *attenuation = ADC_ATTEN_DB_12;
    else {
        JS_ThrowRangeError(ctx, "attenuation must be 0, 2.5, 6, or 12");
        return false;
    }
    return true;
}

static double attenuation_db(adc_atten_t attenuation)
{
    switch (attenuation) {
    case ADC_ATTEN_DB_0: return 0;
    case ADC_ATTEN_DB_2_5: return 2.5;
    case ADC_ATTEN_DB_6: return 6;
    default: return 12;
    }
}

static JSValue js_adc_list(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value; (void)argc; (void)argv;
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    uint32_t index = 0;
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        adc_channel_t channel;
        if (!adc1_pin(pin, &channel)) continue;
        adc_pin_t *state = &s_adc.pins[pin];
        bool allowed = (s_adc.allowed_pin_mask & (UINT64_C(1) << pin)) != 0;
        bool reserved = !state->configured &&
                        esp_gpio_is_reserved(UINT64_C(1) << pin);
        JSValue item = JS_NewObject(ctx);
        if (JS_IsException(item)) goto fail;
        if (JS_SetPropertyStr(ctx, item, "pin", JS_NewInt32(ctx, pin)) < 0 ||
            JS_SetPropertyStr(ctx, item, "available", JS_NewBool(ctx, allowed && !reserved)) < 0 ||
            JS_SetPropertyStr(ctx, item, "configured", JS_NewBool(ctx, state->configured)) < 0 ||
            JS_SetPropertyStr(ctx, item, "attenuation", state->configured ?
                JS_NewFloat64(ctx, attenuation_db(state->attenuation)) : JS_NULL) < 0 ||
            ((!allowed || reserved) && JS_SetPropertyStr(ctx, item, "reason",
                JS_NewString(ctx, allowed ? "reserved" : "not-allowed")) < 0)) {
            JS_FreeValue(ctx, item);
            goto fail;
        }
        if (JS_SetPropertyUint32(ctx, array, index++, item) < 0) goto fail;
    }
    return array;
fail:
    JS_FreeValue(ctx, array);
    return JS_EXCEPTION;
}

static JSValue js_adc_setup(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    int pin;
    adc_atten_t attenuation;
    if (!get_pin(ctx, argc, argv, &pin) ||
        !parse_options(ctx, argc > 1 ? argv[1] : JS_UNDEFINED, &attenuation)) {
        return JS_EXCEPTION;
    }
    adc_pin_t *state = &s_adc.pins[pin];
    uint64_t mask = UINT64_C(1) << pin;
    if (!(s_adc.allowed_pin_mask & mask)) {
        return JS_ThrowRangeError(ctx, "ADC pin is not allowed");
    }
    if (state->configured) {
        if (state->attenuation == attenuation) return JS_UNDEFINED;
        return JS_ThrowInternalError(ctx, "reset ADC pin before changing attenuation");
    }
    if (esp_gpio_reserve(mask) & mask) {
        return JS_ThrowInternalError(ctx, "ADC pin is reserved");
    }
    bool new_unit = !s_adc.unit;
    esp_err_t err = ESP_OK;
    if (new_unit) {
        adc_oneshot_unit_init_cfg_t config = {.unit_id = ADC_UNIT_1};
        err = adc_oneshot_new_unit(&config, &s_adc.unit);
    }
    adc_channel_t channel;
    adc1_pin(pin, &channel);
    if (err == ESP_OK) {
        adc_oneshot_chan_cfg_t config = {
            .atten = attenuation, .bitwidth = ADC_BITWIDTH_12,
        };
        err = adc_oneshot_config_channel(s_adc.unit, channel, &config);
    }
    if (err != ESP_OK) {
        if (new_unit && s_adc.unit) {
            adc_oneshot_del_unit(s_adc.unit);
            s_adc.unit = NULL;
        }
        esp_gpio_revoke(mask);
        return throw_driver(ctx, "adc.setup", err);
    }
    *state = (adc_pin_t) {
        .configured = true, .channel = channel, .attenuation = attenuation,
    };
    ++s_adc.configured_count;
    return JS_UNDEFINED;
}

static JSValue read_pin(JSContext *ctx, int argc, JSValueConst *argv, bool voltage)
{
    int pin;
    if (!get_pin(ctx, argc, argv, &pin)) return JS_EXCEPTION;
    adc_pin_t *state = &s_adc.pins[pin];
    if (!state->configured) {
        return JS_ThrowInternalError(ctx, "ADC pin is not configured");
    }
    if (voltage && !state->calibration) {
        adc_cali_curve_fitting_config_t config = {
            .unit_id = ADC_UNIT_1, .chan = state->channel,
            .atten = state->attenuation, .bitwidth = ADC_BITWIDTH_12,
        };
        esp_err_t err = adc_cali_create_scheme_curve_fitting(&config, &state->calibration);
        if (err != ESP_OK) return throw_driver(ctx, "adc.readVoltage calibration", err);
    }
    int result;
    esp_err_t err = adc_oneshot_read(s_adc.unit, state->channel, &result);
    if (err == ESP_OK && voltage) {
        err = adc_cali_raw_to_voltage(state->calibration, result, &result);
    }
    if (err != ESP_OK) return throw_driver(ctx, voltage ? "adc.readVoltage" : "adc.read", err);
    return JS_NewInt32(ctx, result);
}

static JSValue js_adc_read(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    return read_pin(ctx, argc, argv, false);
}

static JSValue js_adc_read_voltage(JSContext *ctx, JSValueConst this_value,
                                    int argc, JSValueConst *argv)
{
    (void)this_value;
    return read_pin(ctx, argc, argv, true);
}

static void reset_pin(int pin)
{
    adc_pin_t *state = &s_adc.pins[pin];
    if (!state->configured) return;
    if (state->calibration) adc_cali_delete_scheme_curve_fitting(state->calibration);
    /* gpio_reset_pin also releases the IDF pin reservation. */
    gpio_reset_pin(pin);
    memset(state, 0, sizeof(*state));
    if (--s_adc.configured_count == 0) {
        adc_oneshot_del_unit(s_adc.unit);
        s_adc.unit = NULL;
    }
}

static JSValue js_adc_reset(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    int pin;
    if (!get_pin(ctx, argc, argv, &pin)) return JS_EXCEPTION;
    bool configured = s_adc.pins[pin].configured;
    reset_pin(pin);
    return JS_NewBool(ctx, configured);
}

static const JSCFunctionListEntry adc_methods[] = {
    JS_CFUNC_DEF("list", 0, js_adc_list),
    JS_CFUNC_DEF("setup", 1, js_adc_setup),
    JS_CFUNC_DEF("read", 1, js_adc_read),
    JS_CFUNC_DEF("readVoltage", 1, js_adc_read_voltage),
    JS_CFUNC_DEF("reset", 1, js_adc_reset),
};

static esp_err_t adc_module_init(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)runtime; (void)opaque;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue adc = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(adc)) {
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, adc);
        return ESP_ERR_NO_MEM;
    }
    if (JS_SetPropertyFunctionList(ctx, adc, adc_methods,
                                  sizeof(adc_methods) / sizeof(adc_methods[0])) < 0) {
        JS_FreeValue(ctx, adc);
        JS_FreeValue(ctx, global);
        return ESP_FAIL;
    }
    int result = JS_SetPropertyStr(ctx, global, "adc", adc);
    JS_FreeValue(ctx, global);
    return result < 0 ? ESP_FAIL : ESP_OK;
}

static void adc_module_deinit(js_runtime_t *runtime, JSContext *ctx, void *opaque)
{
    (void)runtime; (void)ctx; (void)opaque;
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) reset_pin(pin);
}

esp_err_t js_adc_register(const js_adc_config_t *config)
{
    if (!config || !config->allowed_pin_mask) return ESP_ERR_INVALID_ARG;
    if (s_registered) return ESP_ERR_INVALID_STATE;
    s_adc.allowed_pin_mask = config->allowed_pin_mask;
    esp_err_t err = js_runtime_register_module(adc_module_init, adc_module_deinit, NULL);
    if (err == ESP_OK) s_registered = true;
    return err;
}
#else
esp_err_t js_adc_register(const js_adc_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
