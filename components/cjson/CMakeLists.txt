idf_component_register(SRCS "cJSON.c" "cJSON_Utils.c"
                       INCLUDE_DIRS ".")

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    CJSON_NESTING_LIMIT=${CONFIG_CJSON_NESTING_LIMIT}
    CJSON_CIRCULAR_LIMIT=${CONFIG_CJSON_CIRCULAR_LIMIT})
