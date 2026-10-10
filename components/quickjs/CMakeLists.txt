idf_component_register(
    SRCS "upstream/quickjs.c" "upstream/cutils.c" "upstream/libregexp.c"
         "upstream/libunicode.c" "upstream/dtoa.c"
    INCLUDE_DIRS "upstream"
    PRIV_REQUIRES heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    _GNU_SOURCE CONFIG_VERSION="2025-09-13")
# Match upstream's aliasing assumptions; third-party warnings must not fail IDF builds.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -fno-strict-aliasing -Wno-error -Wno-format -Wno-unused-parameter)
target_link_libraries(${COMPONENT_LIB} PUBLIC m)
