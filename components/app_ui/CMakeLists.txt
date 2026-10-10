idf_component_register(
    SRCS "app_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES app_http_server
    EMBED_TXTFILES "assets/index.html" "assets/style.css" "assets/app.js"
)
