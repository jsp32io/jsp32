#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "quickjs.h"
#define CONFIG_JS_ADC_ENABLED 1
#define GPIO_NUM_MAX 49
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define ESP_ERR_TIMEOUT 5
typedef int esp_err_t;
typedef int adc_channel_t;
typedef int adc_unit_t;
typedef enum { ADC_ATTEN_DB_0, ADC_ATTEN_DB_2_5, ADC_ATTEN_DB_6, ADC_ATTEN_DB_12 } adc_atten_t;
#define ADC_UNIT_1 0
#define ADC_BITWIDTH_12 12
typedef void *adc_oneshot_unit_handle_t;
typedef void *adc_cali_handle_t;
typedef struct { int unit_id; } adc_oneshot_unit_init_cfg_t;
typedef struct { adc_atten_t atten; int bitwidth; } adc_oneshot_chan_cfg_t;
typedef struct { int unit_id, chan; adc_atten_t atten; int bitwidth; } adc_cali_curve_fitting_config_t;
typedef struct js_runtime js_runtime_t;
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *, JSContext *, void *);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *, JSContext *, void *);
esp_err_t js_runtime_register_module(js_runtime_module_init_fn, js_runtime_module_deinit_fn, void *);
const char *esp_err_to_name(esp_err_t);
esp_err_t adc_oneshot_io_to_channel(int, adc_unit_t *, adc_channel_t *);
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t *);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *);
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *, adc_cali_handle_t *);
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t);
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int, int *);
uint64_t esp_gpio_reserve(uint64_t);
uint64_t esp_gpio_revoke(uint64_t);
bool esp_gpio_is_reserved(uint64_t);
esp_err_t gpio_reset_pin(int);
