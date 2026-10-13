#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "quickjs.h"
#define CONFIG_JS_GPIO_ENABLED 1
#define CONFIG_JS_GPIO_MAX_WATCHERS 32
#define GPIO_NUM_MAX 49
#define GPIO_IS_VALID_GPIO(pin) ((pin) >= 0 && (pin) < 49 && ((pin) <= 21 || (pin) >= 26))
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) (GPIO_IS_VALID_GPIO(pin) && (pin) != 46)
#define BIT64(pin) (UINT64_C(1) << (pin))
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_NOT_SUPPORTED 4
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_ANYEDGE 3
#define ESP_TIMER_TASK 0
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdFALSE 0
#define pdTRUE 1
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
#define portYIELD_FROM_ISR() ((void)0)
typedef int esp_err_t;
typedef int gpio_num_t;
typedef int portMUX_TYPE;
typedef int BaseType_t;
typedef int TickType_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
typedef struct mock_timer *esp_timer_handle_t;
typedef struct { void (*callback)(void *); void *arg; int dispatch_method; const char *name; } esp_timer_create_args_t;
typedef struct js_runtime js_runtime_t;
typedef esp_err_t (*js_runtime_module_init_fn)(js_runtime_t *, JSContext *, void *);
typedef void (*js_runtime_module_deinit_fn)(js_runtime_t *, JSContext *, void *);
esp_err_t js_runtime_register_module(js_runtime_module_init_fn, js_runtime_module_deinit_fn, void *);
void js_runtime_call(js_runtime_t *, JSValueConst, JSValueConst, int, JSValueConst *);
const char *esp_err_to_name(esp_err_t);
uint64_t esp_gpio_reserve(uint64_t);
bool esp_gpio_is_reserved(uint64_t);
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, int);
int gpio_get_level(int);
esp_err_t gpio_reset_pin(int);
esp_err_t gpio_intr_enable(int);
esp_err_t gpio_intr_disable(int);
esp_err_t gpio_set_intr_type(int, int);
esp_err_t gpio_isr_handler_add(int, void (*)(void *), void *);
esp_err_t gpio_isr_handler_remove(int);
esp_err_t gpio_install_isr_service(int);
void gpio_uninstall_isr_service(void);
esp_err_t esp_timer_create(const esp_timer_create_args_t *, esp_timer_handle_t *);
esp_err_t esp_timer_delete(esp_timer_handle_t);
esp_err_t esp_timer_stop(esp_timer_handle_t);
esp_err_t esp_timer_start_once(esp_timer_handle_t, uint64_t);
int64_t esp_timer_get_time(void);
