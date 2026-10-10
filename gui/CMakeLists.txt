cmake_minimum_required(VERSION 3.21)
project(jsp32_gui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(JSONCPP REQUIRED IMPORTED_TARGET jsoncpp)
pkg_check_modules(ARCHIVE REQUIRED IMPORTED_TARGET libarchive)
find_package(OpenGL REQUIRED)

if(WIN32)
    set(DISCOVERY_SOURCE src/discovery_windows.cpp)
    set(FONT_SOURCE src/font_windows.cpp)
else()
    pkg_check_modules(AVAHI REQUIRED IMPORTED_TARGET avahi-client)
    pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
    set(DISCOVERY_SOURCE src/discovery_linux.cpp)
    set(FONT_SOURCE src/font_linux.cpp)
endif()

set(IMGUI vendor/imgui)
add_library(imgui STATIC
    ${IMGUI}/imgui.cpp
    ${IMGUI}/imgui_draw.cpp
    ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp
    ${IMGUI}/misc/cpp/imgui_stdlib.cpp
    ${IMGUI}/backends/imgui_impl_sdl3.cpp
    ${IMGUI}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC ${IMGUI} ${IMGUI}/backends ${IMGUI}/misc/cpp)
target_link_libraries(imgui PUBLIC PkgConfig::SDL3 OpenGL::GL)

add_library(jsp32_gui_core STATIC
    src/model.cpp src/api.cpp src/register_stream.cpp src/plugin_config.cpp ${DISCOVERY_SOURCE} src/plugin_zip.cpp)
target_include_directories(jsp32_gui_core PUBLIC src)
target_link_libraries(jsp32_gui_core PUBLIC
    PkgConfig::CURL PkgConfig::JSONCPP PkgConfig::ARCHIVE)
if(WIN32)
    target_link_libraries(jsp32_gui_core PUBLIC dnsapi ws2_32)
    target_compile_definitions(jsp32_gui_core PUBLIC UNICODE _UNICODE _WIN32_WINNT=0x0A00)
else()
    target_link_libraries(jsp32_gui_core PUBLIC PkgConfig::AVAHI)
endif()

if(WIN32)
    add_executable(jsp32-gui WIN32 src/main.cpp src/app.cpp src/app_registers.cpp src/ui_devices.cpp src/ui_overview.cpp src/ui_registers.cpp src/ui_plugins.cpp ${FONT_SOURCE})
else()
    add_executable(jsp32-gui src/main.cpp src/app.cpp src/app_registers.cpp src/ui_devices.cpp src/ui_overview.cpp src/ui_registers.cpp src/ui_plugins.cpp ${FONT_SOURCE})
endif()
target_link_libraries(jsp32-gui PRIVATE imgui jsp32_gui_core)
if(WIN32)
    target_link_libraries(jsp32-gui PRIVATE user32)
else()
    target_link_libraries(jsp32-gui PRIVATE PkgConfig::FONTCONFIG)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(jsp32-gui-model-test tests/model_test.cpp)
    target_link_libraries(jsp32-gui-model-test PRIVATE jsp32_gui_core)
    add_test(NAME model COMMAND jsp32-gui-model-test)
    add_executable(jsp32-gui-plugin-config-test tests/plugin_config_test.cpp)
    target_link_libraries(jsp32-gui-plugin-config-test PRIVATE jsp32_gui_core)
    add_test(NAME plugin-config COMMAND jsp32-gui-plugin-config-test)
    add_executable(jsp32-gui-register-stream-test tests/register_stream_test.cpp)
    target_link_libraries(jsp32-gui-register-stream-test PRIVATE jsp32_gui_core)
    add_test(NAME register-stream COMMAND jsp32-gui-register-stream-test)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME register-stream-http COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/register_stream_http_test.py
            $<TARGET_FILE:jsp32-gui-register-stream-test>)
        set_tests_properties(register-stream-http PROPERTIES TIMEOUT 25)
    endif()
endif()

if(WIN32)
    get_filename_component(JSP32_TOOLCHAIN_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    install(TARGETS jsp32-gui RUNTIME_DEPENDENCY_SET jsp32_runtime RUNTIME DESTINATION .)
    install(RUNTIME_DEPENDENCY_SET jsp32_runtime
        PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
        POST_EXCLUDE_REGEXES ".*[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\].*"
        DIRECTORIES "${JSP32_TOOLCHAIN_BIN}"
        RUNTIME DESTINATION .)
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGE_FILE_NAME "jsp32-gui-windows-x64")
    include(CPack)
endif()
