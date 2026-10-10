idf_component_register(
    SRCS "js_i2c.c"
    INCLUDE_DIRS "include"
    REQUIRES js_runtime quickjs esp_driver_i2c esp_driver_gpio esp_hw_support
)
