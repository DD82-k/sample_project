@echo off
REM ESP-IDF build helper — sets environment and builds the project
REM Author: Claude

set PATH=D:\esp32_s3\idf\Espressif\tools\xtensa-esp32s3-elf\esp-12.2.0_20230208\xtensa-esp32s3-elf\bin;D:\esp32_s3\idf\Espressif\tools\ninja\1.10.2;%PATH%
set IDF_TARGET=esp32s3

echo === Configuring ===
cmake -G Ninja -DCMAKE_MAKE_PROGRAM=D:\esp32_s3\idf\Espressif\tools\ninja\1.10.2\ninja.exe -DPYTHON_DEPS_CHECKED=1 -DPYTHON=D:\esp32_s3\idf\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe -DESP_PLATFORM=1 -DCCACHE_ENABLE=0 -B build

if %ERRORLEVEL% NEQ 0 (
    echo CMake configure failed!
    exit /b 1
)

echo === Building ===
ninja -C build
if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)

echo === Build complete ===
