idf_component_register(
    SRCS "ota_http.c"
    INCLUDE_DIRS "include"
    REQUIRES app_ota app_http_server esp_http_server cjson
             esp_system
)
