cmake_minimum_required(VERSION 3.16)

# Bootloader sub-project inherits this
set(CMAKE_MAKE_PROGRAM "D:/esp32_s3/idf/Espressif/tools/ninja/1.10.2/ninja.exe" CACHE INTERNAL "")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(main)
