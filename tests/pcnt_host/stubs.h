#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"

#define CONFIG_JS_PCNT_ENABLED 1
#define GPIO_NUM_MAX 49
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49 && ((pin) <= 21 || (pin) >= 26))
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_SUPPORTED 4
typedef int esp_err_t;
typedef int gpio_pull_mode_t;
#define GPIO_FLOATING 0
#define GPIO_PULLUP_ONLY 1
#define GPIO_PULLDOWN_ONLY 2
#define GPIO_MODE_INPUT 1
#define GPIO_INTR_DISABLE 0
typedef struct mock_unit *pcnt_unit_handle_t;
typedef struct mock_channel *pcnt_channel_handle_t;
typedef int pcnt_channel_edge_action_t;
#define PCNT_CHANNEL_EDGE_ACTION_HOLD 0
#define PCNT_CHANNEL_EDGE_ACTION_INCREASE 1
#define PCNT_CHANNEL_EDGE_ACTION_DECREASE 2
#define PCNT_CHANNEL_LEVEL_ACTION_KEEP 0
#define PCNT_CHANNEL_LEVEL_ACTION_INVERSE 1
typedef struct {
    int low_limit, high_limit;
    struct { unsigned accum_count: 1; } flags;
} pcnt_unit_config_t;
typedef struct {
    int edge_gpio_num, level_gpio_num;
} pcnt_chan_config_t;
typedef struct { uint32_t max_glitch_ns; } pcnt_glitch_filter_config_t;
typedef struct js_runtime js_runtime_t;
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *, JSContext *, void *);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *, JSContext *, void *);
esp_err_t js_runtime_register_module(js_runtime_module_init_fn, js_runtime_module_deinit_fn, void *);
const char *esp_err_to_name(esp_err_t);
uint64_t esp_gpio_reserve(uint64_t);
uint64_t esp_gpio_revoke(uint64_t);
esp_err_t gpio_set_pull_mode(int, gpio_pull_mode_t);
esp_err_t gpio_reset_pin(int);
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *, pcnt_unit_handle_t *);
esp_err_t pcnt_del_unit(pcnt_unit_handle_t);
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t, const pcnt_glitch_filter_config_t *);
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t, int);
esp_err_t pcnt_new_channel(pcnt_unit_handle_t, const pcnt_chan_config_t *, pcnt_channel_handle_t *);
esp_err_t pcnt_del_channel(pcnt_channel_handle_t);
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t, int, int);
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t, int, int);
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t);
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t);
esp_err_t pcnt_unit_start(pcnt_unit_handle_t);
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t);
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t);
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t, int *);
int64_t esp_timer_get_time(void);
