idf_component_register(SRCS "app_mqtt.c" "mqtt_report.c" "mqtt_http.c"
    INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "."
    REQUIRES app_registers app_http_server cjson
    PRIV_REQUIRES nvs_flash esp_timer esp_hw_support esp-tls mqtt)
