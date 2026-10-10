cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3" CACHE STRING "Target chip")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jsp32)
