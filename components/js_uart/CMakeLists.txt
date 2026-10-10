idf_component_register(
    SRCS "js_uart.c"
    INCLUDE_DIRS "include"
    REQUIRES js_runtime quickjs esp_driver_uart esp_driver_gpio esp_hal_uart esp_timer
             esp_hw_support
)
