idf_component_register(SRCS "reg_http.c" "reg_http_filter.c" "reg_http_connection.c" INCLUDE_DIRS "include"
    REQUIRES app_registers app_http_server esp_http_server cjson)
