idf_component_register(
    SRCS "js_adc.c"
    INCLUDE_DIRS "include"
    REQUIRES js_runtime quickjs esp_adc esp_driver_gpio esp_hw_support
)
