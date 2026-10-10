idf_component_register(SRCS "js_telemetry.c"
    INCLUDE_DIRS "include" REQUIRES js_runtime app_mqtt
    PRIV_REQUIRES quickjs)
