#include <stddef.h>
#include <stdlib.h>

#include "app_filesystem.h"
#include "app_nvs.h"
#include "app_ota.h"
#include "app_registers.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "app_http_server.h"
#include "app_ui.h"
#include "device_discovery.h"
#include "fs_http.h"
#include "js_http.h"
#include "js_fs.h"
#include "js_gpio.h"
#include "js_pcnt.h"
#include "js_adc.h"
#include "js_i2c.h"
#include "js_registers.h"
#include "js_net.h"
#include "js_uart.h"
#include "js_runtime.h"
#include "network_manager.h"
#include "nvs_http.h"
#include "ota_http.h"
#include "reg_http.h"
#include "modbus_tcp.h"
#include "app_mqtt.h"
#include "js_telemetry.h"
#include "sdkconfig.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    app_registers_t *registers = NULL;
    ESP_ERROR_CHECK(app_registers_init(&registers));
    app_nvs_t *nvs_store = NULL;
    ESP_ERROR_CHECK(app_nvs_init(&nvs_store));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    app_http_server_t *http_server = NULL;
    ESP_ERROR_CHECK(app_http_server_start(&http_server));
    ESP_ERROR_CHECK(app_ui_start(http_server));

#if CONFIG_REG_HTTP_ENABLED
    reg_http_t *reg_http = NULL;
    reg_http_config_t reg_http_config = {
        .http_server = http_server,
        .registers = registers,
    };
    ESP_ERROR_CHECK(reg_http_start(&reg_http_config, &reg_http));
#endif

    app_ota_t *ota = NULL;
    ESP_ERROR_CHECK(app_ota_init(&ota));
    ota_http_t *ota_http = NULL;
#if CONFIG_OTA_HTTP_ENABLED
    ota_http_config_t ota_http_config = {
        .http_server = http_server,
        .ota = ota,
    };
    ESP_ERROR_CHECK(ota_http_start(&ota_http_config, &ota_http));
#endif

    nvs_http_t *nvs_http = NULL;
#if CONFIG_NVS_HTTP_ENABLED
    nvs_http_config_t nvs_http_config = {
        .http_server = http_server,
        .store = nvs_store,
    };
    ESP_ERROR_CHECK(nvs_http_start(&nvs_http_config, &nvs_http));
#endif

    network_manager_t *network_manager = NULL;
    network_manager_config_t network_config = {
        .http_server = http_server,
    };
    ESP_ERROR_CHECK(network_manager_init(&network_config, &network_manager));
    err = device_discovery_start(CONFIG_APP_HTTP_SERVER_PORT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Device discovery unavailable: %s",
                 esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(network_manager_start(network_manager));

    modbus_tcp_t *modbus_server = NULL;
    err = modbus_tcp_start(registers, &modbus_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Modbus TCP unavailable: %s", esp_err_to_name(err));
    }

    app_mqtt_t *mqtt_service = NULL;
    err = app_mqtt_start(registers, http_server, &mqtt_service);
    if (err != ESP_OK) ESP_LOGE(TAG, "MQTT unavailable: %s", esp_err_to_name(err));
    else ESP_ERROR_CHECK(js_telemetry_register(mqtt_service));

    app_filesystem_t *filesystem = NULL;
    ESP_ERROR_CHECK(app_filesystem_mount(&filesystem));
    fs_http_t *fs_http = NULL;
#if CONFIG_FS_HTTP_ENABLED
    fs_http_config_t fs_http_config = {
        .http_server = http_server,
        .filesystem = filesystem,
    };
    ESP_ERROR_CHECK(fs_http_start(&fs_http_config, &fs_http));
#endif
    char *script = NULL;
    size_t script_size = 0;
    ESP_ERROR_CHECK(app_filesystem_read_file(filesystem, "/main.js",
                                             &script, &script_size));

    ESP_ERROR_CHECK(js_fs_register(filesystem));

#if CONFIG_JS_ADC_ENABLED
    js_adc_config_t adc_config = JS_ADC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(js_adc_register(&adc_config));
#endif

#if CONFIG_JS_GPIO_ENABLED
    js_gpio_config_t gpio_config = JS_GPIO_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(js_gpio_register(&gpio_config));
#endif

#if CONFIG_JS_PCNT_ENABLED
    js_pcnt_config_t pcnt_config = JS_PCNT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(js_pcnt_register(&pcnt_config));
#endif

#if CONFIG_JS_UART_ENABLED
    js_uart_config_t uart_config = JS_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(js_uart_register(&uart_config));
#endif

#if CONFIG_JS_I2C_ENABLED
    js_i2c_config_t i2c_config = JS_I2C_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(js_i2c_register(&i2c_config));
#endif

#if CONFIG_JS_REGISTERS_ENABLED
    ESP_ERROR_CHECK(js_registers_register(registers));
#endif

#if CONFIG_JS_NET_ENABLED
    ESP_ERROR_CHECK(js_net_register());
#endif

    js_runtime_t *runtime = NULL;
    err = js_runtime_start(script, script_size, "/main.js", filesystem, &runtime);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start JavaScript runtime: %s", esp_err_to_name(err));
        free(script);
        return;
    }

    js_http_t *js_http = NULL;
    js_http_config_t js_http_config = {
        .http_server = http_server,
        .runtime = runtime,
    };
    ESP_ERROR_CHECK(js_http_start(&js_http_config, &js_http));

    ESP_LOGI(TAG, "Network, JavaScript EventLoop and HTTP APIs started "
             "(%p, %p, %p, %p, %p, %p)", (void *)network_manager,
             (void *)runtime, (void *)js_http, (void *)fs_http,
             (void *)nvs_http, (void *)ota_http);
}
