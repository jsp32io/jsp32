idf_component_register(
    SRCS "js_http.c" "js_http_console.c" "js_http_eval.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "."
    REQUIRES app_http_server js_runtime cjson
)
