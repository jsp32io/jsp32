idf_component_register(
    SRCS "fs_http.c"
    INCLUDE_DIRS "include"
    REQUIRES app_filesystem app_http_server esp_http_server cjson
)
