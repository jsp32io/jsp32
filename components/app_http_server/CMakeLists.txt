idf_component_register(
    SRCS "app_http_server.c" "system_info.c" "man.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "." "${CMAKE_CURRENT_BINARY_DIR}"
    REQUIRES esp_http_server esp_system esp_timer esp_hw_support esp_psram spi_flash
             esp_partition esp_app_format cjson heap
)

# Rebuild embedded manuals when pages change; reconfigure when pages are added/removed.
file(GLOB_RECURSE MAN_SOURCES CONFIGURE_DEPENDS "${PROJECT_DIR}/man/*.md")
set(MAN_HEADER "${CMAKE_CURRENT_BINARY_DIR}/man_pages.h")
add_custom_command(
    OUTPUT "${MAN_HEADER}"
    COMMAND ${PYTHON} "${PROJECT_DIR}/tools/embed_man.py"
            "${PROJECT_DIR}/man" "${MAN_HEADER}"
    DEPENDS "${PROJECT_DIR}/tools/embed_man.py" ${MAN_SOURCES}
    VERBATIM
)
add_custom_target(man_pages DEPENDS "${MAN_HEADER}")
add_dependencies(${COMPONENT_LIB} man_pages)
