idf_component_register(
    SRCS "js_runtime.c" "js_modules.c" "js_console.c" "js_eval.c" "js_event.c"
         "js_listener.c" "js_timer.c" "js_request.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES quickjs app_filesystem esp_timer esp_psram
)
