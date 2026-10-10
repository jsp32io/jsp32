#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stubs.h"
#include "js_pcnt.h"

struct mock_unit {
    bool enabled, running;
    int count, channels, watch_points;
    pcnt_channel_handle_t channel_ref[2];
};
struct mock_channel {
    pcnt_unit_handle_t unit;
    pcnt_chan_config_t config;
    int pos, neg, high, low;
};
static js_runtime_module_init_fn module_init;
static js_runtime_module_deinit_fn module_deinit;
static uint64_t reserved;
static int live_units, live_channels, fail_new_unit, fail_channel;
static struct mock_channel last_channels[2];
static int channel_index;
static pcnt_unit_handle_t current_unit;

esp_err_t js_runtime_register_module(js_runtime_module_init_fn init, js_runtime_module_deinit_fn deinit, void *opaque)
{ (void)opaque; module_init = init; module_deinit = deinit; return ESP_OK; }
const char *esp_err_to_name(esp_err_t err)
{ (void)err; return "MOCK_ERROR"; }
uint64_t esp_gpio_reserve(uint64_t mask)
{ uint64_t old = reserved & mask; reserved |= mask; return old; }
uint64_t esp_gpio_revoke(uint64_t mask)
{ uint64_t old = reserved & mask; reserved &= ~mask; return old; }
esp_err_t gpio_set_pull_mode(int pin, gpio_pull_mode_t pull)
{ assert(reserved & (UINT64_C(1) << pin)); assert(pull >= 0 && pull <= 2); return ESP_OK; }
esp_err_t gpio_reset_pin(int pin)
{ esp_gpio_revoke(UINT64_C(1) << pin); return ESP_OK; }
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *config, pcnt_unit_handle_t *out)
{
    assert(config->low_limit == -32768 && config->high_limit == 32767);
    assert(config->flags.accum_count);
    if (fail_new_unit) return ESP_FAIL;
    *out = calloc(1, sizeof(**out)); assert(*out); ++live_units;
    current_unit = *out;
    channel_index = 0;
    return ESP_OK;
}
esp_err_t pcnt_del_unit(pcnt_unit_handle_t unit)
{ assert(unit && !unit->enabled && !unit->channels); if (current_unit == unit) current_unit = NULL; free(unit); --live_units; return ESP_OK; }
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t unit, const pcnt_glitch_filter_config_t *config)
{ assert(unit && config->max_glitch_ns); return ESP_OK; }
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t unit, int point)
{ assert(point == -32768 || point == 32767); ++unit->watch_points; return ESP_OK; }
esp_err_t pcnt_new_channel(pcnt_unit_handle_t unit, const pcnt_chan_config_t *config, pcnt_channel_handle_t *out)
{
    if (fail_channel) return ESP_FAIL;
    *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->unit = unit;
    (*out)->config = *config;
    unit->channel_ref[unit->channels] = *out;
    ++unit->channels; ++live_channels;
    return ESP_OK;
}
esp_err_t pcnt_del_channel(pcnt_channel_handle_t channel)
{ assert(channel); last_channels[channel_index++] = *channel; channel->unit->channel_ref[channel->unit->channels - 1] = NULL; --channel->unit->channels; free(channel); --live_channels; return ESP_OK; }
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t channel, int pos, int neg)
{ channel->pos = pos; channel->neg = neg; return ESP_OK; }
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t channel, int high, int low)
{ channel->high = high; channel->low = low; return ESP_OK; }
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t unit)
{ assert(unit->watch_points == 2); unit->enabled = true; return ESP_OK; }
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t unit)
{ assert(unit->enabled && !unit->running); unit->enabled = false; return ESP_OK; }
esp_err_t pcnt_unit_start(pcnt_unit_handle_t unit)
{ assert(unit->enabled && !unit->running); unit->running = true; return ESP_OK; }
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t unit)
{ assert(unit->running); unit->running = false; return ESP_OK; }
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t unit)
{ unit->count = 0; return ESP_OK; }
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t unit, int *count)
{ *count = unit->count; return ESP_OK; }
int64_t esp_timer_get_time(void) { return 1234567; }

static void transition(int old_a, int old_b, int new_a, int new_b)
{
    assert(current_unit && current_unit->running);
    for (int i = 0; i < current_unit->channels; ++i) {
        struct mock_channel *ch = current_unit->channel_ref[i];
        int old_edge = ch->config.edge_gpio_num == 4 ? old_a : old_b;
        int new_edge = ch->config.edge_gpio_num == 4 ? new_a : new_b;
        if (old_edge == new_edge) continue;
        int action = new_edge ? ch->pos : ch->neg;
        int level = ch->config.level_gpio_num == 4 ? new_a : new_b;
        if (ch->config.level_gpio_num >= 0 &&
            (level ? ch->high : ch->low) == PCNT_CHANNEL_LEVEL_ACTION_INVERSE) {
            if (action == PCNT_CHANNEL_EDGE_ACTION_INCREASE) action = PCNT_CHANNEL_EDGE_ACTION_DECREASE;
            else if (action == PCNT_CHANNEL_EDGE_ACTION_DECREASE) action = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
        }
        current_unit->count += action == PCNT_CHANNEL_EDGE_ACTION_INCREASE ? 1 :
                               action == PCNT_CHANNEL_EDGE_ACTION_DECREASE ? -1 : 0;
    }
}

static void cycle_forward(void)
{
    transition(0, 0, 1, 0); transition(1, 0, 1, 1);
    transition(1, 1, 0, 1); transition(0, 1, 0, 0);
}

static void cycle_reverse(void)
{
    transition(0, 0, 0, 1); transition(0, 1, 1, 1);
    transition(1, 1, 1, 0); transition(1, 0, 0, 0);
}

static void evaluate(JSContext *ctx, const char *source)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "pcnt-test", JS_EVAL_TYPE_GLOBAL);
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
    js_pcnt_config_t config = {.allowed_pin_mask = (1ULL << 4) | (1ULL << 5)};
    assert(js_pcnt_register(NULL) == ESP_ERR_INVALID_ARG);
    assert(js_pcnt_register(&config) == ESP_OK);
    assert(js_pcnt_register(&config) == ESP_ERR_INVALID_STATE);
    JSRuntime *runtime = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(runtime);
    assert(module_init(NULL, ctx, NULL) == ESP_OK);
    evaluate(ctx,
        "function assert(v) { if (!v) throw Error('assertion'); }"
        "function throws(f, type) { let caught = false; try { f(); } catch(e) { caught = true; assert(e instanceof type); } assert(caught); }"
        "for (const o of [undefined, null, [], 1, {foo:1}, {pin:4, extra:1}]) throws(() => pcnt.open(o), TypeError);"
        "for (const pin of [-1, 22, 6]) throws(() => pcnt.open({pin}), RangeError);"
        "for (const pin of [1.5, NaN, Infinity, '4']) throws(() => pcnt.open({pin}), TypeError);"
        "throws(() => pcnt.open({pin:4, edge:'x'}), RangeError);"
        "throws(() => pcnt.openQuadrature({a:4,b:4}), RangeError);"
        "throws(() => pcnt.openQuadrature({a:4,b:5,resolution:3}), RangeError);"
        "throws(() => pcnt.open({get pin(){throw Error('getter');}}), Error);"
        "globalThis.c = pcnt.open({pin:4, edge:'both', filterNs:1000});"
        "assert(c.read() === 0); assert(c.sample().timestampUs === 1234567);"
        "c.start(); c.start(); c.stop(); c.stop(); c.clear();"
        "throws(() => pcnt.open({pin:4}), InternalError);"
        "c.close(); c.close(); throws(() => c.read(), InternalError);");
    assert(!live_units && !live_channels && !reserved);
    assert(last_channels[0].pos == PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    assert(last_channels[0].neg == PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    fail_new_unit = 1;
    evaluate(ctx, "throws(() => pcnt.open({pin:4}), InternalError);");
    assert(!reserved && !live_units); fail_new_unit = 0;
    fail_channel = 1;
    evaluate(ctx, "throws(() => pcnt.open({pin:4}), InternalError);");
    assert(!reserved && !live_units); fail_channel = 0;
    evaluate(ctx, "globalThis.q = pcnt.openQuadrature({a:4,b:5}); q.start();");
    cycle_forward(); assert(current_unit->count == 4);
    cycle_reverse(); assert(current_unit->count == 0);
    assert(reserved == ((1ULL << 4) | (1ULL << 5)));
    evaluate(ctx, "throws(() => pcnt.open({pin:5}), InternalError); q.close();");
    assert(!reserved && !live_units && !live_channels);
    assert(last_channels[0].config.edge_gpio_num == 4 && last_channels[0].config.level_gpio_num == 5);
    assert(last_channels[0].pos == PCNT_CHANNEL_EDGE_ACTION_INCREASE &&
           last_channels[0].neg == PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    assert(last_channels[0].high == PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    assert(last_channels[1].config.edge_gpio_num == 5 && last_channels[1].config.level_gpio_num == 4);
    assert(last_channels[1].pos == PCNT_CHANNEL_EDGE_ACTION_DECREASE &&
           last_channels[1].neg == PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    evaluate(ctx, "globalThis.q1 = pcnt.openQuadrature({a:4,b:5,resolution:1}); q1.start();");
    cycle_forward(); assert(current_unit->count == 1);
    cycle_reverse(); assert(current_unit->count == 0);
    evaluate(ctx, "q1.close();");
    assert(last_channels[0].neg == PCNT_CHANNEL_EDGE_ACTION_HOLD);
    evaluate(ctx, "globalThis.q2 = pcnt.openQuadrature({a:4,b:5,resolution:2}); q2.start();");
    cycle_forward(); assert(current_unit->count == 2);
    cycle_reverse(); assert(current_unit->count == 0);
    module_deinit(NULL, ctx, NULL);
    assert(!reserved && !live_units && !live_channels);
    JS_FreeContext(ctx); JS_FreeRuntime(runtime);
    puts("PCNT host tests passed (real QuickJS, mocked ESP-IDF)");
}
