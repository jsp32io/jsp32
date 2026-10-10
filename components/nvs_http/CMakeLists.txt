idf_component_register(
    SRCS "nvs_http.c"
    INCLUDE_DIRS "include"
    REQUIRES app_nvs app_http_server esp_http_server cjson
             mbedtls esp_partition
)
