#include "js_gpio.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_private/esp_gpio_reserve.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "js_event.h"
#include "js_runtime.h"
#include "quickjs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#if CONFIG_JS_GPIO_ENABLED

#define GPIO_EVENT_RAW 1
#define GPIO_EVENT_STABLE 2
#define GPIO_DEBOUNCE_MAX_MS 60000

typedef enum {
    WATCH_RISING = 1,
    WATCH_FALLING = 2,
    WATCH_BOTH = WATCH_RISING | WATCH_FALLING,
} watch_edge_t;

typedef struct js_gpio_module js_gpio_module_t;

typedef struct {
    js_gpio_module_t *module;
    gpio_num_t pin;
    esp_timer_handle_t debounce_timer;
    int64_t last_edge_us;
    uint16_t interrupt_epoch;
    uint16_t generation;
    uint32_t debounce_ms;
    uint8_t pull;
    uint8_t initial;
    uint8_t level;
    bool configured;
    bool output;
    bool interrupt_active;
} pin_state_t;

typedef struct gpio_subscription {
    uint32_t id;
    gpio_num_t pin;
    watch_edge_t edge;
    JSValue callback;
    struct gpio_subscription *next;
} gpio_subscription_t;

struct js_gpio_module {
    js_runtime_t *runtime;
    JSContext *ctx;
    uint64_t allowed_pin_mask;
    portMUX_TYPE lock;
    pin_state_t pins[GPIO_NUM_MAX];
    gpio_subscription_t *subscriptions;
    uint32_t next_subscription_id;
    size_t subscription_count;
    bool isr_service_owned;
    bool initialized;
};

static js_gpio_module_t s_gpio = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};
static bool s_registered;

static bool pin_number_valid(int32_t pin)
{
    return pin >= 0 && pin < GPIO_NUM_MAX && GPIO_IS_VALID_GPIO(pin);
}

static bool pin_allowed(const js_gpio_module_t *module, gpio_num_t pin)
{
    return pin < 64 && (module->allowed_pin_mask & BIT64(pin)) != 0;
}

static bool pin_reserved(const pin_state_t *state)
{
    return !state->configured && esp_gpio_is_reserved(BIT64(state->pin));
}

static JSValue throw_esp_error(JSContext *ctx, const char *operation,
                               esp_err_t err)
{
    return JS_ThrowInternalError(ctx, "%s failed: %s", operation,
                                 esp_err_to_name(err));
}

static bool get_pin(JSContext *ctx, int argc, JSValueConst *argv, int index,
                    gpio_num_t *out_pin)
{
    if (argc <= index || !JS_IsNumber(argv[index])) {
        JS_ThrowTypeError(ctx, "pin must be an integer");
        return false;
    }
    double number = 0;
    if (JS_ToFloat64(ctx, &number, argv[index])) return false;
    if (!isfinite(number) || floor(number) != number ||
        number < INT32_MIN || number > INT32_MAX) {
        JS_ThrowTypeError(ctx, "pin must be an integer");
        return false;
    }
    int32_t pin = (int32_t)number;
    if (!pin_number_valid(pin)) {
        JS_ThrowRangeError(ctx, "invalid GPIO pin");
        return false;
    }
    *out_pin = (gpio_num_t)pin;
    return true;
}

static bool known_setup_field(const char *name)
{
    return strcmp(name, "mode") == 0 || strcmp(name, "pull") == 0 ||
           strcmp(name, "initial") == 0 ||
           strcmp(name, "debounceMs") == 0;
}

static bool validate_setup_fields(JSContext *ctx, JSValueConst options)
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
        bool known = known_setup_field(name);
        if (!known) {
            JS_ThrowTypeError(ctx, "unknown GPIO setup option: %s", name);
            valid = false;
        }
        JS_FreeCString(ctx, name);
        if (!valid) break;
    }
    JS_FreePropertyEnum(ctx, properties, count);
    return valid;
}

static bool get_string_option(JSContext *ctx, JSValueConst options,
                              const char *name, const char **out_value,
                              JSValue *out_property)
{
    JSValue property = JS_GetPropertyStr(ctx, options, name);
    if (JS_IsException(property)) return false;
    *out_property = property;
    if (JS_IsUndefined(property)) {
        *out_value = NULL;
        return true;
    }
    if (!JS_IsString(property)) {
        JS_ThrowTypeError(ctx, "%s must be a string", name);
        return false;
    }
    *out_value = JS_ToCString(ctx, property);
    return *out_value != NULL;
}

static bool parse_binary_value(JSContext *ctx, JSValueConst value,
                               uint8_t *out_value)
{
    if (JS_IsBool(value)) {
        int result = JS_ToBool(ctx, value);
        if (result < 0) return false;
        *out_value = result ? 1 : 0;
        return true;
    }
    if (!JS_IsNumber(value)) {
        JS_ThrowTypeError(ctx, "value must be 0, 1, or boolean");
        return false;
    }
    double number = 0;
    if (JS_ToFloat64(ctx, &number, value) ||
        (number != 0 && number != 1)) {
        JS_ThrowRangeError(ctx, "value must be 0 or 1");
        return false;
    }
    *out_value = number == 1 ? 1 : 0;
    return true;
}

typedef struct {
    bool output;
    uint8_t pull;
    uint8_t initial;
    uint32_t debounce_ms;
} setup_options_t;

static bool parse_setup_options(JSContext *ctx, JSValueConst options,
                                setup_options_t *out)
{
    if (!JS_IsObject(options) || JS_IsArray(ctx, options) ||
        JS_IsFunction(ctx, options)) {
        JS_ThrowTypeError(ctx, "options must be an object");
        return false;
    }
    if (!validate_setup_fields(ctx, options)) return false;
    memset(out, 0, sizeof(*out));

    JSValue mode_property = JS_UNDEFINED;
    const char *mode = NULL;
    if (!get_string_option(ctx, options, "mode", &mode, &mode_property)) {
        JS_FreeValue(ctx, mode_property);
        return false;
    }
    if (!mode) {
        JS_FreeValue(ctx, mode_property);
        JS_ThrowTypeError(ctx, "mode is required");
        return false;
    }
    if (strcmp(mode, "input") == 0) out->output = false;
    else if (strcmp(mode, "output") == 0) out->output = true;
    else {
        JS_FreeCString(ctx, mode);
        JS_FreeValue(ctx, mode_property);
        JS_ThrowRangeError(ctx, "mode must be input or output");
        return false;
    }
    JS_FreeCString(ctx, mode);
    JS_FreeValue(ctx, mode_property);

    JSValue pull_property = JS_UNDEFINED;
    const char *pull = NULL;
    if (!get_string_option(ctx, options, "pull", &pull, &pull_property)) {
        JS_FreeValue(ctx, pull_property);
        return false;
    }
    if (pull) {
        if (out->output) {
            JS_FreeCString(ctx, pull);
            JS_FreeValue(ctx, pull_property);
            JS_ThrowTypeError(ctx, "pull is only valid for input mode");
            return false;
        }
        if (strcmp(pull, "none") == 0) out->pull = 0;
        else if (strcmp(pull, "up") == 0) out->pull = 1;
        else if (strcmp(pull, "down") == 0) out->pull = 2;
        else {
            JS_FreeCString(ctx, pull);
            JS_FreeValue(ctx, pull_property);
            JS_ThrowRangeError(ctx, "pull must be none, up, or down");
            return false;
        }
        JS_FreeCString(ctx, pull);
    }
    JS_FreeValue(ctx, pull_property);

    JSValue initial = JS_GetPropertyStr(ctx, options, "initial");
    if (JS_IsException(initial)) return false;
    if (!JS_IsUndefined(initial)) {
        if (!out->output) {
            JS_FreeValue(ctx, initial);
            JS_ThrowTypeError(ctx, "initial is only valid for output mode");
            return false;
        }
        if (!parse_binary_value(ctx, initial, &out->initial)) {
            JS_FreeValue(ctx, initial);
            return false;
        }
    }
    JS_FreeValue(ctx, initial);

    JSValue debounce = JS_GetPropertyStr(ctx, options, "debounceMs");
    if (JS_IsException(debounce)) return false;
    if (!JS_IsUndefined(debounce)) {
        if (out->output) {
            JS_FreeValue(ctx, debounce);
            JS_ThrowTypeError(ctx,
                              "debounceMs is only valid for input mode");
            return false;
        }
        if (!JS_IsNumber(debounce)) {
            JS_FreeValue(ctx, debounce);
            JS_ThrowTypeError(ctx, "debounceMs must be an integer");
            return false;
        }
        double number = 0;
        if (JS_ToFloat64(ctx, &number, debounce) || !isfinite(number) ||
            floor(number) != number || number < 0 ||
            number > GPIO_DEBOUNCE_MAX_MS) {
            JS_FreeValue(ctx, debounce);
            JS_ThrowRangeError(ctx, "debounceMs must be between 0 and 60000");
            return false;
        }
        out->debounce_ms = (uint32_t)number;
    }
    JS_FreeValue(ctx, debounce);
    return true;
}

static bool same_configuration(const pin_state_t *state,
                               const setup_options_t *options)
{
    return state->configured && state->output == options->output &&
           state->pull == options->pull &&
           state->initial == options->initial &&
           state->debounce_ms == options->debounce_ms;
}

static size_t watchers_for_pin(const js_gpio_module_t *module, gpio_num_t pin)
{
    size_t count = 0;
    for (const gpio_subscription_t *item = module->subscriptions; item;
         item = item->next) {
        if (item->pin == pin) ++count;
    }
    return count;
}

static void interrupt_disable(pin_state_t *state)
{
    if (!state->interrupt_active) return;
    gpio_intr_disable(state->pin);
    gpio_isr_handler_remove(state->pin);
    state->interrupt_active = false;
}

static bool remove_subscription(js_gpio_module_t *module, uint32_t id)
{
    gpio_subscription_t **link = &module->subscriptions;
    while (*link) {
        gpio_subscription_t *item = *link;
        if (item->id == id) {
            gpio_num_t pin = item->pin;
            *link = item->next;
            JS_FreeValue(module->ctx, item->callback);
            free(item);
            --module->subscription_count;
            if (watchers_for_pin(module, pin) == 0) {
                interrupt_disable(&module->pins[pin]);
                if (module->pins[pin].debounce_timer) {
                    esp_timer_stop(module->pins[pin].debounce_timer);
                }
            }
            return true;
        }
        link = &item->next;
    }
    return false;
}

static void remove_pin_subscriptions(js_gpio_module_t *module,
                                     gpio_num_t pin)
{
    interrupt_disable(&module->pins[pin]);
    if (module->pins[pin].debounce_timer) {
        esp_timer_stop(module->pins[pin].debounce_timer);
    }
    gpio_subscription_t **link = &module->subscriptions;
    while (*link) {
        gpio_subscription_t *item = *link;
        if (item->pin == pin) {
            *link = item->next;
            JS_FreeValue(module->ctx, item->callback);
            free(item);
            --module->subscription_count;
            continue;
        }
        link = &item->next;
    }
}

static void debounce_timer_callback(void *argument)
{
    pin_state_t *state = argument;
    js_gpio_module_t *module = state->module;
    if (!module->initialized || !state->configured || state->output) return;

    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&module->lock);
    int64_t last_edge = state->last_edge_us;
    uint16_t epoch = state->interrupt_epoch;
    uint16_t generation = state->generation;
    portEXIT_CRITICAL(&module->lock);
    int64_t delay_us = (int64_t)state->debounce_ms * 1000;
    int64_t remaining = delay_us - (now - last_edge);
    if (remaining > 0) {
        esp_timer_start_once(state->debounce_timer, (uint64_t)remaining);
        return;
    }

    uint32_t level = gpio_get_level(state->pin) ? 1 : 0;
    js_event_t event = {
        .type = JS_EVENT_GPIO,
        .subtype = GPIO_EVENT_STABLE,
        .source = (uint32_t)state->pin,
        .request_id = ((uint32_t)epoch << 16) |
                      ((uint32_t)generation << 1) | level,
        .timestamp_us = last_edge,
    };
    js_event_post(module->runtime, &event, 0);
}

static void gpio_interrupt(void *argument)
{
    pin_state_t *state = argument;
    js_gpio_module_t *module = state->module;
    uint32_t level = gpio_get_level(state->pin) ? 1 : 0;
    portENTER_CRITICAL_ISR(&module->lock);
    uint16_t generation = (state->generation + 1) & 0x7fff;
    if (generation == 0) generation = 1;
    state->generation = generation;
    uint16_t epoch = state->interrupt_epoch;
    int64_t timestamp_us = esp_timer_get_time();
    state->last_edge_us = timestamp_us;
    portEXIT_CRITICAL_ISR(&module->lock);
    js_event_t event = {
        .type = JS_EVENT_GPIO,
        .subtype = GPIO_EVENT_RAW,
        .source = (uint32_t)state->pin,
        .request_id = ((uint32_t)epoch << 16) |
                      ((uint32_t)generation << 1) | level,
        .timestamp_us = timestamp_us,
    };
    BaseType_t wake = pdFALSE;
    js_event_post_from_isr(module->runtime, &event, &wake);
    if (wake == pdTRUE) portYIELD_FROM_ISR();
}

static esp_err_t interrupt_enable(pin_state_t *state)
{
    state->level = gpio_get_level(state->pin) ? 1 : 0;
    portENTER_CRITICAL(&state->module->lock);
    ++state->interrupt_epoch;
    if (state->interrupt_epoch == 0) ++state->interrupt_epoch;
    state->generation = 0;
    portEXIT_CRITICAL(&state->module->lock);
    esp_err_t err = gpio_set_intr_type(state->pin, GPIO_INTR_ANYEDGE);
    if (err == ESP_OK) {
        err = gpio_isr_handler_add(state->pin, gpio_interrupt, state);
    }
    if (err == ESP_OK) err = gpio_intr_enable(state->pin);
    if (err != ESP_OK) {
        gpio_isr_handler_remove(state->pin);
        gpio_intr_disable(state->pin);
        return err;
    }
    state->interrupt_active = true;
    return ESP_OK;
}

static const char *edge_name(watch_edge_t edge)
{
    return edge == WATCH_RISING ? "rising" : "falling";
}

static void emit_edge(js_gpio_module_t *module, pin_state_t *state,
                      uint8_t level, int64_t timestamp_us)
{
    if (level == state->level) return;
    watch_edge_t edge = level ? WATCH_RISING : WATCH_FALLING;
    state->level = level;

    size_t count = 0;
    for (gpio_subscription_t *item = module->subscriptions; item;
         item = item->next) {
        if (item->pin == state->pin && (item->edge & edge)) ++count;
    }
    if (count == 0) return;
    JSValue *callbacks = calloc(count, sizeof(*callbacks));
    if (!callbacks) return;
    size_t index = 0;
    for (gpio_subscription_t *item = module->subscriptions; item;
         item = item->next) {
        if (item->pin == state->pin && (item->edge & edge)) {
            callbacks[index++] = JS_DupValue(module->ctx, item->callback);
        }
    }

    JSValue event = JS_NewObject(module->ctx);
    if (!JS_IsException(event)) {
        JS_SetPropertyStr(module->ctx, event, "pin",
                          JS_NewInt32(module->ctx, state->pin));
        JS_SetPropertyStr(module->ctx, event, "value",
                          JS_NewInt32(module->ctx, level));
        JS_SetPropertyStr(module->ctx, event, "edge",
                          JS_NewString(module->ctx, edge_name(edge)));
        JS_SetPropertyStr(module->ctx, event, "timestampUs",
                          JS_NewFloat64(module->ctx, (double)timestamp_us));
        for (size_t i = 0; i < count; ++i) {
            JSValueConst argv[] = {event};
            js_runtime_call(module->runtime, callbacks[i], JS_UNDEFINED,
                            1, argv);
            JS_FreeValue(module->ctx, callbacks[i]);
        }
        JS_FreeValue(module->ctx, event);
    } else {
        for (size_t i = 0; i < count; ++i) {
            JS_FreeValue(module->ctx, callbacks[i]);
        }
    }
    free(callbacks);
}

static void gpio_event_handler(js_runtime_t *runtime,
                               const js_event_t *event, void *opaque)
{
    (void)runtime;
    js_gpio_module_t *module = opaque;
    if (event->source >= GPIO_NUM_MAX) return;
    pin_state_t *state = &module->pins[event->source];
    if (!state->configured || state->output || !state->interrupt_active) {
        return;
    }
    uint16_t epoch = (uint16_t)(event->request_id >> 16);
    uint16_t generation = (uint16_t)((event->request_id >> 1) & 0x7fff);
    uint8_t level = (uint8_t)(event->request_id & 1);
    portENTER_CRITICAL(&module->lock);
    bool same_epoch = epoch == state->interrupt_epoch;
    portEXIT_CRITICAL(&module->lock);
    if (!same_epoch) return;
    if (event->subtype == GPIO_EVENT_RAW) {
        if (state->debounce_ms == 0) {
            emit_edge(module, state, level, event->timestamp_us);
        } else {
            esp_timer_stop(state->debounce_timer);
            esp_timer_start_once(state->debounce_timer,
                                 (uint64_t)state->debounce_ms * 1000);
        }
    } else if (event->subtype == GPIO_EVENT_STABLE) {
        portENTER_CRITICAL(&module->lock);
        bool current = generation == state->generation &&
                       epoch == state->interrupt_epoch;
        portEXIT_CRITICAL(&module->lock);
        if (current && gpio_get_level(state->pin) == level) {
            emit_edge(module, state, level, event->timestamp_us);
        }
    }
}

static void reset_state(js_gpio_module_t *module, pin_state_t *state)
{
    remove_pin_subscriptions(module, state->pin);
    if (state->debounce_timer) {
        esp_timer_stop(state->debounce_timer);
        esp_timer_delete(state->debounce_timer);
    }
    gpio_reset_pin(state->pin);
    gpio_num_t pin = state->pin;
    memset(state, 0, sizeof(*state));
    state->module = module;
    state->pin = pin;
}

static JSValue js_gpio_list(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    js_gpio_module_t *module = &s_gpio;
    JSValue array = JS_NewArray(ctx);
    if (JS_IsException(array)) return array;
    uint32_t index = 0;
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if (!GPIO_IS_VALID_GPIO(pin)) continue;
        pin_state_t *state = &module->pins[pin];
        bool allowed = pin_allowed(module, pin);
        bool reserved = pin_reserved(state);
        bool available = allowed && !reserved;
        JSValue item = JS_NewObject(ctx);
        if (JS_IsException(item)) {
            JS_FreeValue(ctx, array);
            return item;
        }
        JS_SetPropertyStr(ctx, item, "pin", JS_NewInt32(ctx, pin));
        JS_SetPropertyStr(ctx, item, "available",
                          JS_NewBool(ctx, available));
        JS_SetPropertyStr(ctx, item, "configured",
                          JS_NewBool(ctx, state->configured));
        JS_SetPropertyStr(ctx, item, "mode",
            state->configured
                ? JS_NewString(ctx, state->output ? "output" : "input")
                : JS_NULL);
        if (!available) {
            JS_SetPropertyStr(ctx, item, "reason",
                JS_NewString(ctx, allowed ? "reserved" : "not-allowed"));
        }
        if (JS_SetPropertyUint32(ctx, array, index++, item) < 0) {
            JS_FreeValue(ctx, array);
            return JS_EXCEPTION;
        }
    }
    return array;
}

static JSValue configure_pin(JSContext *ctx, gpio_num_t pin,
                             const setup_options_t *options)
{
    js_gpio_module_t *module = &s_gpio;
    pin_state_t *state = &module->pins[pin];
    if (!pin_allowed(module, pin)) {
        return JS_ThrowRangeError(ctx, "GPIO pin is not allowed");
    }
    if (pin_reserved(state)) {
        return JS_ThrowRangeError(ctx, "GPIO pin is reserved");
    }
    if (options->output && !GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
        return JS_ThrowRangeError(ctx, "GPIO pin does not support output");
    }
    if (same_configuration(state, options)) return JS_UNDEFINED;

    esp_timer_handle_t new_timer = NULL;
    if (!options->output && options->debounce_ms > 0) {
        esp_timer_create_args_t timer_args = {
            .callback = debounce_timer_callback,
            .arg = state,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "js_gpio",
        };
        esp_err_t err = esp_timer_create(&timer_args, &new_timer);
        if (err != ESP_OK) return throw_esp_error(ctx, "gpio.setup", err);
    }

    if (options->output) {
        esp_err_t err = gpio_set_level(pin, options->initial);
        if (err != ESP_OK) {
            if (new_timer) esp_timer_delete(new_timer);
            return throw_esp_error(ctx, "gpio.setup", err);
        }
    }
    gpio_config_t config = {
        .pin_bit_mask = BIT64(pin),
        .mode = options->output ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en = options->pull == 1,
        .pull_down_en = options->pull == 2,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        if (new_timer) esp_timer_delete(new_timer);
        return throw_esp_error(ctx, "gpio.setup", err);
    }
    if (!state->configured && !options->output &&
        (esp_gpio_reserve(BIT64(pin)) & BIT64(pin))) {
        gpio_reset_pin(pin);
        if (new_timer) esp_timer_delete(new_timer);
        return JS_ThrowRangeError(ctx, "GPIO pin became reserved");
    }

    remove_pin_subscriptions(module, pin);
    if (state->debounce_timer) {
        esp_timer_stop(state->debounce_timer);
        esp_timer_delete(state->debounce_timer);
    }
    state->configured = true;
    state->output = options->output;
    state->pull = options->pull;
    state->initial = options->initial;
    state->level = options->output ? options->initial
                                  : (gpio_get_level(pin) ? 1 : 0);
    state->debounce_ms = options->debounce_ms;
    state->debounce_timer = new_timer;
    return JS_UNDEFINED;
}

static JSValue js_gpio_setup(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    gpio_num_t pin;
    if (!get_pin(ctx, argc, argv, 0, &pin)) return JS_EXCEPTION;
    if (argc < 2) return JS_ThrowTypeError(ctx, "options are required");
    setup_options_t options;
    if (!parse_setup_options(ctx, argv[1], &options)) return JS_EXCEPTION;
    return configure_pin(ctx, pin, &options);
}

static JSValue configure_default_input(JSContext *ctx, gpio_num_t pin)
{
    const setup_options_t options = {.debounce_ms = 20};
    return configure_pin(ctx, pin, &options);
}

static JSValue js_gpio_read(JSContext *ctx, JSValueConst this_value,
                            int argc, JSValueConst *argv)
{
    (void)this_value;
    gpio_num_t pin;
    if (!get_pin(ctx, argc, argv, 0, &pin)) return JS_EXCEPTION;
    pin_state_t *state = &s_gpio.pins[pin];
    if (!state->configured) {
        if (JS_IsException(configure_default_input(ctx, pin))) {
            return JS_EXCEPTION;
        }
    }
    int level = state->output ? state->level : gpio_get_level(pin);
    return JS_NewInt32(ctx, level ? 1 : 0);
}

static JSValue js_gpio_write(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    gpio_num_t pin;
    if (!get_pin(ctx, argc, argv, 0, &pin)) return JS_EXCEPTION;
    pin_state_t *state = &s_gpio.pins[pin];
    if (state->configured && !state->output) {
        return JS_ThrowInternalError(ctx,
                                     "GPIO pin is not configured as output");
    }
    if (argc < 2) return JS_ThrowTypeError(ctx, "value is required");
    uint8_t level;
    if (!parse_binary_value(ctx, argv[1], &level)) return JS_EXCEPTION;
    if (!state->configured) {
        const setup_options_t options = {.output = true, .initial = level};
        return configure_pin(ctx, pin, &options);
    }
    esp_err_t err = gpio_set_level(pin, level);
    if (err != ESP_OK) return throw_esp_error(ctx, "gpio.write", err);
    state->level = level;
    return JS_UNDEFINED;
}

static JSValue unsubscribe_function(JSContext *ctx,
                                    JSValueConst this_value, int argc,
                                    JSValueConst *argv, int magic,
                                    JSValue *func_data)
{
    (void)this_value;
    (void)argc;
    (void)argv;
    (void)magic;
    uint32_t id = 0;
    if (JS_ToUint32(ctx, &id, func_data[0])) return JS_EXCEPTION;
    if (s_gpio.initialized) remove_subscription(&s_gpio, id);
    return JS_UNDEFINED;
}

static bool parse_edge(JSContext *ctx, JSValueConst value,
                       watch_edge_t *out_edge)
{
    if (!JS_IsString(value)) {
        JS_ThrowTypeError(ctx, "edge must be a string");
        return false;
    }
    const char *name = JS_ToCString(ctx, value);
    if (!name) return false;
    bool valid = true;
    if (strcmp(name, "rising") == 0) *out_edge = WATCH_RISING;
    else if (strcmp(name, "falling") == 0) *out_edge = WATCH_FALLING;
    else if (strcmp(name, "both") == 0) *out_edge = WATCH_BOTH;
    else valid = false;
    JS_FreeCString(ctx, name);
    if (!valid) {
        JS_ThrowRangeError(ctx, "edge must be rising, falling, or both");
    }
    return valid;
}

static JSValue js_gpio_watch(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    gpio_num_t pin;
    if (!get_pin(ctx, argc, argv, 0, &pin)) return JS_EXCEPTION;
    pin_state_t *state = &s_gpio.pins[pin];
    if (state->configured && state->output) {
        return JS_ThrowInternalError(ctx,
                                     "GPIO pin is not configured as input");
    }
    watch_edge_t edge;
    if (argc < 2) return JS_ThrowTypeError(ctx, "edge is required");
    if (!parse_edge(ctx, argv[1], &edge)) return JS_EXCEPTION;
    if (argc < 3 || !JS_IsFunction(ctx, argv[2])) {
        return JS_ThrowTypeError(ctx, "callback must be a function");
    }
    js_gpio_module_t *module = &s_gpio;
    if (module->subscription_count >= CONFIG_JS_GPIO_MAX_WATCHERS) {
        return JS_ThrowRangeError(ctx, "too many GPIO watchers");
    }
    if (!state->configured &&
        JS_IsException(configure_default_input(ctx, pin))) {
        return JS_EXCEPTION;
    }
    gpio_subscription_t *subscription = calloc(1, sizeof(*subscription));
    if (!subscription) return JS_ThrowOutOfMemory(ctx);
    uint32_t id = ++module->next_subscription_id;
    if (id == 0) id = ++module->next_subscription_id;
    subscription->id = id;
    subscription->pin = pin;
    subscription->edge = edge;
    subscription->callback = JS_DupValue(ctx, argv[2]);

    bool first = watchers_for_pin(module, pin) == 0;
    if (first) {
        esp_err_t err = interrupt_enable(state);
        if (err != ESP_OK) {
            JS_FreeValue(ctx, subscription->callback);
            free(subscription);
            return throw_esp_error(ctx, "gpio.watch", err);
        }
    }
    gpio_subscription_t **tail = &module->subscriptions;
    while (*tail) tail = &(*tail)->next;
    *tail = subscription;
    ++module->subscription_count;

    JSValue data = JS_NewUint32(ctx, id);
    JSValue function = JS_NewCFunctionData(ctx, unsubscribe_function, 0, 0,
                                           1, &data);
    JS_FreeValue(ctx, data);
    if (JS_IsException(function)) {
        remove_subscription(module, id);
    }
    return function;
}

static JSValue js_gpio_reset(JSContext *ctx, JSValueConst this_value,
                             int argc, JSValueConst *argv)
{
    (void)this_value;
    gpio_num_t pin;
    if (!get_pin(ctx, argc, argv, 0, &pin)) return JS_EXCEPTION;
    pin_state_t *state = &s_gpio.pins[pin];
    bool configured = state->configured;
    if (configured) reset_state(&s_gpio, state);
    return JS_NewBool(ctx, configured);
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

static esp_err_t gpio_module_init(js_runtime_t *runtime, JSContext *ctx,
                                  void *opaque)
{
    js_gpio_module_t *module = opaque;
    module->runtime = runtime;
    module->ctx = ctx;
    module->next_subscription_id = 0;
    module->subscriptions = NULL;
    module->subscription_count = 0;
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        module->pins[pin].module = module;
        module->pins[pin].pin = (gpio_num_t)pin;
    }

    esp_err_t err = gpio_install_isr_service(0);
    if (err == ESP_OK) {
        module->isr_service_owned = true;
    } else if (err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    err = js_event_register_handler(runtime, JS_EVENT_GPIO,
                                    gpio_event_handler, module);
    if (err != ESP_OK) {
        if (module->isr_service_owned) gpio_uninstall_isr_service();
        module->isr_service_owned = false;
        return err;
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JSValue gpio = JS_NewObject(ctx);
    if (JS_IsException(global) || JS_IsException(gpio)) {
        JS_FreeValue(ctx, gpio);
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_GPIO);
        if (module->isr_service_owned) gpio_uninstall_isr_service();
        module->isr_service_owned = false;
        return ESP_ERR_NO_MEM;
    }
    if (install_method(ctx, gpio, "list", js_gpio_list, 0) != ESP_OK ||
        install_method(ctx, gpio, "setup", js_gpio_setup, 2) != ESP_OK ||
        install_method(ctx, gpio, "read", js_gpio_read, 1) != ESP_OK ||
        install_method(ctx, gpio, "write", js_gpio_write, 2) != ESP_OK ||
        install_method(ctx, gpio, "watch", js_gpio_watch, 3) != ESP_OK ||
        install_method(ctx, gpio, "reset", js_gpio_reset, 1) != ESP_OK) {
        JS_FreeValue(ctx, gpio);
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_GPIO);
        if (module->isr_service_owned) gpio_uninstall_isr_service();
        module->isr_service_owned = false;
        return ESP_FAIL;
    }
    if (JS_SetPropertyStr(ctx, global, "gpio", gpio) < 0) {
        JS_FreeValue(ctx, global);
        js_event_unregister_handler(runtime, JS_EVENT_GPIO);
        if (module->isr_service_owned) gpio_uninstall_isr_service();
        module->isr_service_owned = false;
        return ESP_FAIL;
    }
    JS_FreeValue(ctx, global);
    module->initialized = true;
    return ESP_OK;
}

static void gpio_module_deinit(js_runtime_t *runtime, JSContext *ctx,
                               void *opaque)
{
    (void)ctx;
    js_gpio_module_t *module = opaque;
    module->initialized = false;
    for (int pin = 0; pin < GPIO_NUM_MAX; ++pin) {
        if (module->pins[pin].configured) {
            reset_state(module, &module->pins[pin]);
        }
    }
    while (module->subscriptions) {
        remove_subscription(module, module->subscriptions->id);
    }
    js_event_unregister_handler(runtime, JS_EVENT_GPIO);
    if (module->isr_service_owned) gpio_uninstall_isr_service();
    module->isr_service_owned = false;
    module->runtime = NULL;
    module->ctx = NULL;
}

esp_err_t js_gpio_register(const js_gpio_config_t *config)
{
    if (!config || config->allowed_pin_mask == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_registered) return ESP_ERR_INVALID_STATE;
    s_gpio.allowed_pin_mask = config->allowed_pin_mask;
    esp_err_t err = js_runtime_register_module(gpio_module_init,
                                               gpio_module_deinit, &s_gpio);
    if (err == ESP_OK) s_registered = true;
    return err;
}

#else

esp_err_t js_gpio_register(const js_gpio_config_t *config)
{
    (void)config;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
