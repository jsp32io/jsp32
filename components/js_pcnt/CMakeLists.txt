idf_component_register(
    SRCS "js_pcnt.c"
    INCLUDE_DIRS "include"
    REQUIRES js_runtime quickjs esp_driver_pcnt esp_driver_gpio esp_hw_support esp_timer
)
