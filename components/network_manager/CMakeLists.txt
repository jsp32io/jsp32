idf_component_register(
    SRCS "network_manager.c" "network_store.c" "network_http.c"
         "network_dns.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES app_http_server esp_wifi esp_event esp_timer esp_netif nvs_flash wifi_tx_power
             cjson lwip
)
