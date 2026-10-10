idf_component_register(SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES app_filesystem fs_http app_ota ota_http
             js_runtime js_http js_fs js_adc js_gpio js_pcnt js_uart js_i2c js_registers js_net
             app_http_server app_ui
             app_nvs app_registers nvs_http reg_http modbus_tcp app_mqtt js_telemetry network_manager
             device_discovery
             nvs_flash esp_netif esp_event)
