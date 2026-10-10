#include "js_pcnt.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "js_runtime.h"
#include "quickjs.h"

#if CONFIG_JS_PCNT_ENABLED
#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"

typedef struct counter {
    uint32_t id;
    uint64_t pin_mask;
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t channels[2];
    bool enabled;
    bool running;
    struct counter *next;
} counter_t;

static struct {
    uint64_t allowed_pin_mask;
    counter_t *counters;
    uint32_t next_id;
} s_pcnt;
static bool s_registered;

typedef struct {
    int a;
    int b;
    int edge;
    int resolution;
    gpio_pull_mode_t pull;
    uint32_t filter_ns;
} open_options_t;

static JSValue driver_error(JSContext *ctx, const char *operation, esp_err_t err)
{
    return JS_ThrowInternalError(ctx, "%s failed: %s", operation,
                                 esp_err_to_name(err));
}

static bool integer_property(JSContext *ctx, JSValueConst object,
                             const char *name, bool required,
                             int64_t minimum, int64_t maximum,
                             int64_t *result)
{
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsException(value)) return false;
    if (JS_IsUndefined(value)) {
        JS_FreeValue(ctx, value);
        if (required) JS_ThrowTypeError(ctx, "%s is required", name);
        return !required;
    }
    double number;
    bool valid = JS_IsNumber(value) && JS_ToFloat64(ctx, &number, value) == 0 &&
                 isfinite(number) && floor(number) == number;
    JS_FreeValue(ctx, value);
    if (!valid) {
        JS_ThrowTypeError(ctx, "%s must be an integer", name);
        return false;
    }
    if (number < (double)minimum || number > (double)maximum) {
        JS_ThrowRangeError(ctx, "%s is out of range", name);
        return false;
    }
    *result = (int64_t)number;
    return true;
}

static bool string_property(JSContext *ctx, JSValueConst object,
                            const char *name, const char *const *choices,
                            size_t count, int *result)
{
    JSValue value = JS_GetPropertyStr(ctx, object, name);
    if (JS_IsException(value)) return false;
    if (JS_IsUndefined(value)) {
        JS_FreeValue(ctx, value);
        return true;
    }
    if (!JS_IsString(value)) {
        JS_FreeValue(ctx, value);
        JS_ThrowTypeError(ctx, "%s must be a string", name);
        return false;
    }
    const char *text = JS_ToCString(ctx, value);
    JS_FreeValue(ctx, value);
    if (!text) return false;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(text, choices[i]) == 0) {
            *result = (int)i;
            found = true;
            break;
        }
    }
    JS_FreeCString(ctx, text);
    if (!found) JS_ThrowRangeError(ctx, "%s has an invalid value", name);
    return found;
}

static bool parse_options(JSContext *ctx, JSValueConst value, bool quadrature,
                          open_options_t *out)
{
    if (!JS_IsObject(value) || JS_IsNull(value) ||
        JS_IsArray(ctx, value) || JS_IsFunction(ctx, value)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return false;
    }
    static const char *const single_names[] = {
        "pin", "edge", "pull", "filterNs"
    };
    static const char *const quad_names[] = {
        "a", "b", "resolution", "pull", "filterNs"
    };
    const char *const *names = quadrature ? quad_names : single_names;
    size_t name_count = quadrature ? 5 : 4;
    JSPropertyEnum *properties = NULL;
    uint32_t property_count = 0;
    if (JS_GetOwnPropertyNames(ctx, &properties, &property_count, value,
                               JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) {
        return false;
    }
    bool valid = true;
    for (uint32_t i = 0; i < property_count; ++i) {
        const char *name = JS_AtomToCString(ctx, properties[i].atom);
        if (!name) { valid = false; break; }
        bool known = false;
        for (size_t j = 0; j < name_count; ++j) {
            if (strcmp(name, names[j]) == 0) { known = true; break; }
        }
        if (!known) JS_ThrowTypeError(ctx, "unknown PCNT option: %s", name);
        JS_FreeCString(ctx, name);
        if (!known) { valid = false; break; }
    }
    JS_FreePropertyEnum(ctx, properties, property_count);
    if (!valid) return false;

    *out = (open_options_t) {
        .a = -1, .b = -1, .edge = 0, .resolution = 4,
        .pull = GPIO_FLOATING, .filter_ns = 0,
    };
    int64_t number = -1;
    if (!integer_property(ctx, value, quadrature ? "a" : "pin", true,
                          0, GPIO_NUM_MAX - 1, &number)) return false;
    out->a = (int)number;
    if (quadrature) {
        if (!integer_property(ctx, value, "b", true, 0,
                              GPIO_NUM_MAX - 1, &number)) return false;
        out->b = (int)number;
        if (out->a == out->b) {
            JS_ThrowRangeError(ctx, "a and b must be different pins");
            return false;
        }
        number = 4;
        if (!integer_property(ctx, value, "resolution", false, 1, 4,
                              &number)) return false;
        if (number != 1 && number != 2 && number != 4) {
            JS_ThrowRangeError(ctx, "resolution must be 1, 2, or 4");
            return false;
        }
        out->resolution = (int)number;
    } else {
        static const char *const edges[] = {"rising", "falling", "both"};
        if (!string_property(ctx, value, "edge", edges, 3, &out->edge)) return false;
    }
    static const char *const pulls[] = {"none", "up", "down"};
    int pull = 0;
    if (!string_property(ctx, value, "pull", pulls, 3, &pull)) return false;
    out->pull = pull == 1 ? GPIO_PULLUP_ONLY :
                pull == 2 ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING;
    number = 0;
    if (!integer_property(ctx, value, "filterNs", false, 0, UINT32_MAX,
                          &number)) return false;
    out->filter_ns = (uint32_t)number;
    return true;
}

static void close_counter(counter_t *counter)
{
    if (counter->running) pcnt_unit_stop(counter->unit);
    if (counter->enabled) pcnt_unit_disable(counter->unit);
    for (int i = 0; i < 2; ++i) {
        if (counter->channels[i]) pcnt_del_channel(counter->channels[i]);
    }
    if (counter->unit) pcnt_del_unit(counter->unit);
    for (int pin = 0; pin < GPIO_NUM_MAX && pin < 64; ++pin) {
        if (counter->pin_mask & (UINT64_C(1) << pin)) gpio_reset_pin(pin);
    }
    free(counter);
}

static counter_t *find_counter(uint32_t id)
{
    for (counter_t *item = s_pcnt.counters; item; item = item->next) {
        if (item->id == id) return item;
    }
    return NULL;
}

static JSValue counter_method(JSContext *ctx, JSValueConst this_value,
                              int argc, JSValueConst *argv, int magic,
                              JSValue *func_data)
{
    (void)this_value; (void)argc; (void)argv;
    uint32_t id;
    if (JS_ToUint32(ctx, &id, func_data[0])) return JS_EXCEPTION;
    counter_t *counter = find_counter(id);
    if (!counter) {
        if (magic == 5) return JS_UNDEFINED;
        return JS_ThrowInternalError(ctx, "PCNT counter is closed");
    }
    esp_err_t err = ESP_OK;
    switch (magic) {
    case 0: /* start */
        if (!counter->running) {
            err = pcnt_unit_start(counter->unit);
            if (err == ESP_OK) counter->running = true;
        }
        break;
    case 1: /* stop */
        if (counter->running) {
            err = pcnt_unit_stop(counter->unit);
            if (err == ESP_OK) counter->running = false;
        }
        break;
    case 2: /* read */
    case 4: { /* sample */
        int count = 0;
        err = pcnt_unit_get_count(counter->unit, &count);
        if (err != ESP_OK) break;
        if (magic == 2) return JS_NewInt32(ctx, count);
        int64_t time_us = esp_timer_get_time();
        JSValue sample = JS_NewObject(ctx);
        if (JS_IsException(sample)) return sample;
        if (JS_SetPropertyStr(ctx, sample, "count", JS_NewInt32(ctx, count)) < 0 ||
            JS_SetPropertyStr(ctx, sample, "timestampUs",
                              JS_NewFloat64(ctx, (double)time_us)) < 0) {
            JS_FreeValue(ctx, sample);
            return JS_EXCEPTION;
        }
        return sample;
    }
    case 3: /* clear */
        err = pcnt_unit_clear_count(counter->unit);
        break;
    case 5: { /* close */
        counter_t **link = &s_pcnt.counters;
        while (*link != counter) link = &(*link)->next;
        *link = counter->next;
        close_counter(counter);
        return JS_UNDEFINED;
    }
    default:
        return JS_ThrowInternalError(ctx, "invalid PCNT method");
    }
    if (err != ESP_OK) return driver_error(ctx, "pcnt counter", err);
    return JS_UNDEFINED;
}

static JSValue make_counter_object(JSContext *ctx, uint32_t id)
{
    static const char *const methods[] = {
        "start", "stop", "read", "clear", "sample", "close"
    };
    JSValue object = JS_NewObject(ctx);
    if (JS_IsException(object)) return object;
    JSValue data = JS_NewUint32(ctx, id);
    if (JS_IsException(data)) { JS_FreeValue(ctx, object); return data; }
    for (int i = 0; i < 6; ++i) {
        JSValue method = JS_NewCFunctionData(ctx, counter_method, 0, i, 1, &data);
        if (JS_IsException(method)) {
            JS_FreeValue(ctx, data);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
        if (JS_SetPropertyStr(ctx, object, methods[i], method) < 0) {
            JS_FreeValue(ctx, data);
            JS_FreeValue(ctx, object);
            return JS_EXCEPTION;
        }
    }
    JS_FreeValue(ctx, data);
    return object;
}

static esp_err_t configure_channel(counter_t *counter, int slot, int edge_pin,
                                   int level_pin, pcnt_channel_edge_action_t pos,
                                   pcnt_channel_edge_action_t neg,
                                   bool inverse_on_high)
{
    pcnt_chan_config_t config = {
        .edge_gpio_num = edge_pin,
        .level_gpio_num = level_pin,
    };
    esp_err_t err = pcnt_new_channel(counter->unit, &config,
                                     &counter->channels[slot]);
    if (err == ESP_OK) {
        err = pcnt_channel_set_edge_action(counter->channels[slot], pos, neg);
    }
    if (err == ESP_OK && level_pin >= 0) {
        err = pcnt_channel_set_level_action(
            counter->channels[slot],
            inverse_on_high ? PCNT_CHANNEL_LEVEL_ACTION_INVERSE :
                              PCNT_CHANNEL_LEVEL_ACTION_KEEP,
            inverse_on_high ? PCNT_CHANNEL_LEVEL_ACTION_KEEP :
                              PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    }
    return err;
}

static JSValue open_counter(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv, int magic)
{
    (void)this_value;
    bool quadrature = magic != 0;
    if (argc < 1) return JS_ThrowTypeError(ctx, "options are required");
    open_options_t options;
    if (!parse_options(ctx, argv[0], quadrature, &options)) return JS_EXCEPTION;
    uint64_t mask = UINT64_C(1) << options.a;
    if (quadrature) mask |= UINT64_C(1) << options.b;
    if (!GPIO_IS_VALID_GPIO(options.a) ||
        (quadrature && !GPIO_IS_VALID_GPIO(options.b)) ||
        (mask & s_pcnt.allowed_pin_mask) != mask) {
        return JS_ThrowRangeError(ctx, "PCNT pin is invalid or not allowed");
    }
    uint64_t old = esp_gpio_reserve(mask);
    if (old & mask) {
        esp_gpio_revoke(mask & ~old);
        return JS_ThrowInternalError(ctx, "PCNT pin is reserved");
    }
    counter_t *counter = calloc(1, sizeof(*counter));
    if (!counter) {
        esp_gpio_revoke(mask);
        return JS_ThrowOutOfMemory(ctx);
    }
    counter->pin_mask = mask;
    esp_err_t err = ESP_OK;
    for (int pin = 0; pin < GPIO_NUM_MAX && pin < 64; ++pin) {
        if (!(mask & (UINT64_C(1) << pin))) continue;
        /* The PCNT driver selects GPIO input mode when creating a channel. */
        err = gpio_set_pull_mode(pin, options.pull);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) {
        pcnt_unit_config_t config = {
            .low_limit = -32768, .high_limit = 32767,
            .flags.accum_count = 1,
        };
        err = pcnt_new_unit(&config, &counter->unit);
    }
    if (err == ESP_OK && options.filter_ns) {
        pcnt_glitch_filter_config_t filter = {
            .max_glitch_ns = options.filter_ns,
        };
        err = pcnt_unit_set_glitch_filter(counter->unit, &filter);
    }
    if (err == ESP_OK) err = pcnt_unit_add_watch_point(counter->unit, -32768);
    if (err == ESP_OK) err = pcnt_unit_add_watch_point(counter->unit, 32767);
    if (err == ESP_OK && !quadrature) {
        pcnt_channel_edge_action_t pos = options.edge == 1 ?
            PCNT_CHANNEL_EDGE_ACTION_HOLD : PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        pcnt_channel_edge_action_t neg = options.edge == 0 ?
            PCNT_CHANNEL_EDGE_ACTION_HOLD : PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        err = configure_channel(counter, 0, options.a, -1, pos, neg, false);
    }
    if (err == ESP_OK && quadrature) {
        err = configure_channel(counter, 0, options.a, options.b,
             PCNT_CHANNEL_EDGE_ACTION_INCREASE,
             options.resolution >= 2 ? PCNT_CHANNEL_EDGE_ACTION_DECREASE :
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD,
             true);
    }
    if (err == ESP_OK && quadrature && options.resolution == 4) {
        err = configure_channel(counter, 1, options.b, options.a,
             PCNT_CHANNEL_EDGE_ACTION_DECREASE,
             PCNT_CHANNEL_EDGE_ACTION_INCREASE, true);
    }
    if (err == ESP_OK) err = pcnt_unit_enable(counter->unit);
    if (err == ESP_OK) counter->enabled = true;
    if (err == ESP_OK) err = pcnt_unit_clear_count(counter->unit);
    if (err != ESP_OK) {
        close_counter(counter);
        return driver_error(ctx, "pcnt.open", err);
    }
    uint32_t id = ++s_pcnt.next_id;
    if (!id) id = ++s_pcnt.next_id;
    counter->id = id;
    JSValue object = make_counter_object(ctx, id);
    if (JS_IsException(object)) {
        close_counter(counter);
        return object;
    }
    counter->next = s_pcnt.counters;
    s_pcnt.counters = counter;
    return object;
}

static JSValue js_pcnt_open(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    return open_counter(ctx, this_value, argc, argv, 0);
}

static JSValue js_pcnt_open_quadrature(JSContext *ctx, JSValueConst this_value,
                                       int argc, JSValueConst *argv)
{
    return open_counter(ctx, this_value, argc, argv, 1);
}

static const JSCFunctionListEntry pcnt_methods[] = {
    JS_CFUNC_DEF("open", 1, js_pcnt_open),
    JS_CFUNC_DEF("openQuadrature", 1, js_pcnt_open_quadrature),
};

static esp_err_t pcnt_module_init(js_runtime_t *runtime, JSContext *ctx,
                                  void *opaque)
{
    (void)runtime; (void)opaque;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue pcnt = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(pcnt)) {
        JS_FreeValue(ctx, global);
        JS_FreeValue(ctx, pcnt);
        return ESP_ERR_NO_MEM;
    }
    if (JS_SetPropertyFunctionList(ctx, pcnt, pcnt_methods,
                                  sizeof(pcnt_methods) / sizeof(pcnt_methods[0])) < 0) {
        JS_FreeValue(ctx, pcnt);
        JS_FreeValue(ctx, global);
        return ESP_FAIL;
    }
    int result = JS_SetPropertyStr(ctx, global, "pcnt", pcnt);
    JS_FreeValue(ctx, global);
    return result < 0 ? ESP_FAIL : ESP_OK;
}

static void pcnt_module_deinit(js_runtime_t *runtime, JSContext *ctx,
                               void *opaque)
{
    (void)runtime; (void)ctx; (void)opaque;
    while (s_pcnt.counters) {
        counter_t *counter = s_pcnt.counters;
        s_pcnt.counters = counter->next;
        close_counter(counter);
    }
}

esp_err_t js_pcnt_register(const js_pcnt_config_t *config)
{
    if (!config || !config->allowed_pin_mask) return ESP_ERR_INVALID_ARG;
    if (s_registered) return ESP_ERR_INVALID_STATE;
    s_pcnt.allowed_pin_mask = config->allowed_pin_mask;
    esp_err_t err = js_runtime_register_module(pcnt_module_init,
                                                pcnt_module_deinit, NULL);
    if (err == ESP_OK) s_registered = true;
    return err;
}
#else
esp_err_t js_pcnt_register(const js_pcnt_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif
