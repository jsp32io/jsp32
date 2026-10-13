#include <assert.h>
#include <stdio.h>
#include "stubs.h"
#include "../../components/js_gpio/js_gpio.c"

struct mock_timer { uint64_t delay; };
static uint64_t reserved;
static int levels[GPIO_NUM_MAX], configs, timer_count, sets, fail_config;
static gpio_config_t last_config;
static js_event_t posted;
esp_err_t js_runtime_register_module(js_runtime_module_init_fn init, js_runtime_module_deinit_fn deinit, void *opaque)
{ (void)init; (void)deinit; (void)opaque; return ESP_OK; }
void js_runtime_call(js_runtime_t *runtime, JSValueConst function, JSValueConst receiver, int argc, JSValueConst *argv)
{ (void)runtime; JSValue result = JS_Call(s_gpio.ctx, function, receiver, argc, argv); assert(!JS_IsException(result)); JS_FreeValue(s_gpio.ctx, result); }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "MOCK_ERROR"; }
uint64_t esp_gpio_reserve(uint64_t mask) { uint64_t old = reserved & mask; reserved |= mask; return old; }
bool esp_gpio_is_reserved(uint64_t mask) { return (reserved & mask) != 0; }
esp_err_t gpio_config(const gpio_config_t *config)
{ if (fail_config) return ESP_FAIL; last_config = *config; ++configs; if (config->mode == GPIO_MODE_OUTPUT) reserved |= config->pin_bit_mask; return ESP_OK; }
esp_err_t gpio_set_level(int pin, int level) { levels[pin] = level; ++sets; return ESP_OK; }
int gpio_get_level(int pin) { return levels[pin]; }
esp_err_t gpio_reset_pin(int pin) { reserved &= ~BIT64(pin); return ESP_OK; }
esp_err_t gpio_intr_enable(int pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_intr_disable(int pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_set_intr_type(int pin, int type) { (void)pin; (void)type; return ESP_OK; }
esp_err_t gpio_isr_handler_add(int pin, void (*handler)(void *), void *arg)
{ (void)pin; (void)handler; (void)arg; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(int pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
void gpio_uninstall_isr_service(void) {}
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{ (void)args; *handle = calloc(1, sizeof(**handle)); assert(*handle); ++timer_count; return ESP_OK; }
esp_err_t esp_timer_delete(esp_timer_handle_t handle) { free(handle); --timer_count; return ESP_OK; }
esp_err_t esp_timer_stop(esp_timer_handle_t handle) { handle->delay = 0; return ESP_OK; }
esp_err_t esp_timer_start_once(esp_timer_handle_t handle, uint64_t delay) { handle->delay = delay; return ESP_OK; }
int64_t esp_timer_get_time(void) { return 100000; }
esp_err_t js_event_post(js_runtime_t *runtime, const js_event_t *event, TickType_t wait)
{ (void)runtime; (void)wait; posted = *event; return ESP_OK; }
esp_err_t js_event_post_from_isr(js_runtime_t *runtime, const js_event_t *event, BaseType_t *wake)
{ (void)wake; return js_event_post(runtime, event, 0); }
esp_err_t js_event_register_handler(js_runtime_t *runtime, uint16_t type, js_event_handler_fn handler, void *opaque)
{ (void)runtime; (void)type; (void)handler; (void)opaque; return ESP_OK; }
esp_err_t js_event_unregister_handler(js_runtime_t *runtime, uint16_t type)
{ (void)runtime; (void)type; return ESP_OK; }

static void evaluate(JSContext *ctx, const char *source)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "gpio-test", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "FAIL: %s\n%s\n", source, message);
        abort();
    }
    JS_FreeValue(ctx, result);
}

int main(void)
{
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    s_gpio.allowed_pin_mask = UINT64_MAX & ~BIT64(10);
    assert(gpio_module_init(NULL, ctx, &s_gpio) == ESP_OK);
    evaluate(ctx, "function assert(v) { if (!v) throw Error('assertion'); }"
        "function throws(f, type) { let caught = false; try { f(); } catch(e) { caught = true; assert(e instanceof type); } assert(caught); }"
        "throws(() => gpio.write(2), TypeError); throws(() => gpio.write(2, 2), RangeError);"
        "throws(() => gpio.watch(2), TypeError); throws(() => gpio.watch(2, 'bad', () => {}), RangeError);"
        "throws(() => gpio.watch(2, 'both', null), TypeError); assert(!gpio.reset(2));");
    assert(configs == 0 && timer_count == 0 && sets == 0);
    levels[2] = 1;
    evaluate(ctx, "assert(gpio.read(2) === 1); throws(() => gpio.write(2, 0), InternalError);");
    assert(configs == 1 && s_gpio.pins[2].debounce_ms == 20 && timer_count == 1);
    assert(last_config.mode == GPIO_MODE_INPUT && !last_config.pull_up_en && !last_config.pull_down_en);
    levels[2] = 0;
    evaluate(ctx, "assert(gpio.read(2) === 0); globalThis.events = 0; globalThis.stop = gpio.watch(2, 'both', () => events++);");
    assert(configs == 1);
    levels[2] = 1;
    gpio_interrupt(&s_gpio.pins[2]);
    gpio_event_handler(NULL, &posted, &s_gpio);
    assert(s_gpio.pins[2].debounce_timer->delay == 20000);
    evaluate(ctx, "assert(events === 0); assert(gpio.read(2) === 1);");
    s_gpio.pins[2].last_edge_us = 80000;
    debounce_timer_callback(&s_gpio.pins[2]);
    gpio_event_handler(NULL, &posted, &s_gpio);
    evaluate(ctx, "assert(events === 1); stop(); stop(); assert(gpio.reset(2)); gpio.write(2, true); assert(gpio.read(2) === 1);"
        "throws(() => gpio.watch(2, 'both', () => {}), InternalError);");
    assert(configs == 2 && sets == 1 && timer_count == 0 && s_gpio.pins[2].initial == 1);
    levels[2] = 0;
    evaluate(ctx, "assert(gpio.read(2) === 1); gpio.write(2, false); assert(gpio.read(2) === 0);"
        "gpio.setup(3, {mode:'input', pull:'up', debounceMs:35});"
        "globalThis.stop3 = gpio.watch(3, 'both', () => {}); gpio.read(3);"
        "gpio.setup(3, {mode:'input', pull:'up', debounceMs:35});");
    assert(configs == 3 && s_gpio.pins[3].pull == 1 && s_gpio.pins[3].debounce_ms == 35 && s_gpio.subscription_count == 1);
    evaluate(ctx, "gpio.setup(3, {mode:'input'});");
    assert(configs == 4 && s_gpio.pins[3].debounce_ms == 0 && s_gpio.subscription_count == 0);
    evaluate(ctx, "globalThis.stop4 = gpio.watch(4, 'both', () => {});");
    assert(s_gpio.pins[4].debounce_ms == 20 && s_gpio.subscription_count == 1);
    reserved |= BIT64(5);
    evaluate(ctx, "for (const pin of [5, 10]) {"
        "throws(() => gpio.read(pin), RangeError); throws(() => gpio.write(pin, 1), RangeError);"
        "throws(() => gpio.watch(pin, 'both', () => {}), RangeError); assert(!gpio.reset(pin)); }"
        "throws(() => gpio.write(46, 1), RangeError); assert(!gpio.reset(46));");
    fail_config = 1;
    int before_timers = timer_count;
    evaluate(ctx, "throws(() => gpio.read(6), InternalError); assert(!gpio.reset(6));"
        "throws(() => gpio.watch(6, 'both', () => {}), InternalError); assert(!gpio.reset(6));");
    assert(timer_count == before_timers);
    fail_config = 0;
    reserved &= ~BIT64(5);
    gpio_module_deinit(NULL, ctx, &s_gpio);
    assert(timer_count == 0 && reserved == 0 && s_gpio.subscription_count == 0);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    puts("GPIO host tests passed");
}
