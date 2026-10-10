idf_component_register(
    SRCS "js_fs.c"
    INCLUDE_DIRS "include"
    REQUIRES app_filesystem js_runtime quickjs
)
