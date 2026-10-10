#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "js_adc.h"

static js_runtime_module_init_fn module_init;
static js_runtime_module_deinit_fn module_deinit;
static uint64_t reserved;
static int units, calibrations, samples;
static int fail_new, fail_config, fail_calibration, fail_read, fail_voltage;
esp_err_t js_runtime_register_module(js_runtime_module_init_fn init, js_runtime_module_deinit_fn deinit, void *opaque)
{ (void)opaque; module_init = init; module_deinit = deinit; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err) { (void)err; return "MOCK_ERROR"; }
esp_err_t adc_oneshot_io_to_channel(int pin, adc_unit_t *unit, adc_channel_t *channel)
{ if (pin < 1 || pin > 20) return ESP_ERR_INVALID_ARG; *unit = pin <= 10 ? 0 : 1; *channel = (pin - 1) % 10; return ESP_OK; }
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *config, adc_oneshot_unit_handle_t *handle)
{ assert(config->unit_id == ADC_UNIT_1); if (fail_new) return ESP_FAIL; assert(units == 0); ++units; *handle = &units; return ESP_OK; }
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t handle)
{ assert(handle == &units && units == 1); --units; return ESP_OK; }
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t handle, adc_channel_t channel, const adc_oneshot_chan_cfg_t *config)
{ assert(handle && channel >= 0 && channel < 10 && config->bitwidth == 12); return fail_config ? ESP_FAIL : ESP_OK; }
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t handle, adc_channel_t channel, int *raw)
{ (void)channel; assert(handle && units == 1); ++samples; *raw = 2048; return fail_read ? ESP_ERR_TIMEOUT : ESP_OK; }
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *config, adc_cali_handle_t *handle)
{ assert(config->bitwidth == 12); if (fail_calibration) return ESP_ERR_NOT_SUPPORTED; *handle = malloc(1); assert(*handle); ++calibrations; return ESP_OK; }
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t handle)
{ assert(handle && calibrations > 0); free(handle); --calibrations; return ESP_OK; }
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t handle, int raw, int *mv)
{ assert(handle && raw == 2048); *mv = 1650; return fail_voltage ? ESP_FAIL : ESP_OK; }
uint64_t esp_gpio_reserve(uint64_t mask) { uint64_t old = reserved & mask; reserved |= mask; return old; }
uint64_t esp_gpio_revoke(uint64_t mask) { uint64_t old = reserved & mask; reserved &= ~mask; return old; }
bool esp_gpio_is_reserved(uint64_t mask) { return (reserved & mask) != 0; }
esp_err_t gpio_reset_pin(int pin) { esp_gpio_revoke(UINT64_C(1) << pin); return ESP_OK; }

static void evaluate(JSContext *ctx, const char *source)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "adc-test", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "FAIL: %s\n%s\n", source, message);
        JS_FreeCString(ctx, message); JS_FreeValue(ctx, error);
        abort();
    }
    JS_FreeValue(ctx, result);
}

int main(void)
{
    js_adc_config_t config = {.allowed_pin_mask = 0x3fe}; /* GPIO 1..9 */
    assert(js_adc_register(NULL) == ESP_ERR_INVALID_ARG);
    assert(js_adc_register(&config) == ESP_OK);
    assert(js_adc_register(&config) == ESP_ERR_INVALID_STATE);
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    assert(module_init(NULL, ctx, NULL) == ESP_OK);
    evaluate(ctx,
        "function assert(v) { if (!v) throw Error('assertion'); }"
        "function throws(f, type) { let caught = false; try { f(); } catch(e) { caught = true; assert(e instanceof type); } assert(caught); }"
        "assert(adc.list().length === 10);"
        "assert(adc.list()[9].reason === 'not-allowed');"
        "for (const p of [undefined, null, '1', true, NaN, Infinity, 1.5]) throws(() => adc.setup(p), TypeError);"
        "for (const p of [-1, 0, 11, 1e100]) throws(() => adc.setup(p), RangeError);"
        "for (const o of [null, [], 1, () => {}, {foo:1}]) throws(() => adc.setup(1,o), TypeError);"
        "throws(() => adc.setup(1,{attenuation:'12'}), TypeError);"
        "for (const a of [11, -1, NaN, Infinity]) throws(() => adc.setup(1,{attenuation:a}), RangeError);"
        "throws(() => adc.setup(1,{get attenuation(){throw Error('getter');}}), Error);"
        "throws(() => adc.setup(10), RangeError);"
        "throws(() => adc.read(1), InternalError); assert(adc.reset(1) === false);");
    assert(!units && !reserved);
    reserved = 1ULL << 2;
    evaluate(ctx, "assert(adc.list()[1].reason === 'reserved'); throws(() => adc.setup(2), InternalError); assert(!adc.reset(2));");
    assert(reserved == (1ULL << 2)); reserved = 0;
    fail_new = 1;
    evaluate(ctx, "throws(() => adc.setup(1), InternalError);");
    assert(!reserved && !units); fail_new = 0;
    fail_config = 1;
    evaluate(ctx, "throws(() => adc.setup(1), InternalError);");
    assert(!reserved && !units); fail_config = 0;
    evaluate(ctx, "assert(adc.setup(1) === undefined); adc.setup(1); assert(adc.list()[0].attenuation === 12); throws(() => adc.setup(1,{attenuation:6}), InternalError); assert(adc.read(1) === 2048);");
    assert(units == 1 && samples == 1);
    fail_config = 1;
    evaluate(ctx, "throws(() => adc.setup(2), InternalError); assert(adc.read(1) === 2048);");
    assert(units == 1 && reserved == (1ULL << 1)); fail_config = 0;
    fail_calibration = 1;
    evaluate(ctx, "throws(() => adc.readVoltage(1), InternalError); assert(adc.read(1) === 2048);");
    assert(!calibrations); fail_calibration = 0;
    evaluate(ctx, "assert(adc.readVoltage(1) === 1650); assert(adc.readVoltage(1) === 1650); adc.setup(2,{attenuation:2.5});");
    assert(calibrations == 1 && units == 1);
    fail_read = 1;
    evaluate(ctx, "throws(() => adc.read(1), InternalError); throws(() => adc.readVoltage(1), InternalError);");
    fail_read = 0; fail_voltage = 1;
    evaluate(ctx, "throws(() => adc.readVoltage(1), InternalError);"); fail_voltage = 0;
    evaluate(ctx, "assert(adc.reset(1)); assert(!adc.reset(1)); assert(adc.read(2) === 2048); assert(adc.reset(2));");
    assert(!units && !reserved && !calibrations);
    evaluate(ctx, "for (const attenuation of [0,2.5,6,12]) { adc.setup(1,{attenuation}); assert(adc.list()[0].attenuation === attenuation); adc.reset(1); } adc.setup(1); adc.readVoltage(1); adc.setup(2); adc.readVoltage(2);");
    module_deinit(NULL, ctx, NULL);
    assert(!units && !reserved && !calibrations);
    JS_FreeContext(ctx);
    ctx = JS_NewContext(runtime);
    assert(module_init(NULL, ctx, NULL) == ESP_OK);
    evaluate(ctx, "adc.setup(1); adc.readVoltage(1);");
    module_deinit(NULL, ctx, NULL);
    assert(!units && !reserved && !calibrations);
    JS_FreeContext(ctx); JS_FreeRuntime(runtime);
    puts("ADC host tests passed (real QuickJS, mocked ESP-IDF)");
}
