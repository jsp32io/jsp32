idf_component_register(
    SRCS "js_registers.c"
    INCLUDE_DIRS "include"
    REQUIRES app_registers js_runtime quickjs
)
