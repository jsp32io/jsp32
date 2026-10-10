idf_component_register(SRCS "modbus_tcp.c" "modbus_tcp_protocol.c"
    INCLUDE_DIRS "include" REQUIRES app_registers nvs_flash
    PRIV_REQUIRES freertos esp_timer lwip)
