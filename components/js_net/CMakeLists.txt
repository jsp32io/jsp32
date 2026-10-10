idf_component_register(
    SRCS "js_net.c" "socket_worker.c" "socket_tls.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES js_runtime quickjs
    PRIV_REQUIRES lwip esp_timer freertos mbedtls
)
