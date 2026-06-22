@echo off
set PATH=D:\esp32_s3\idf\Espressif\tools\xtensa-esp32s3-elf\esp-12.2.0_20230208\xtensa-esp32s3-elf\bin;D:\esp32_s3\idf\Espressif\tools\ninja\1.10.2;%PATH%
set IDF_TARGET=esp32s3
set CMAKE_MAKE_PROGRAM=D:\esp32_s3\idf\Espressif\tools\ninja\1.10.2\ninja.exe
cd /d C:\Users\Lenovo\Desktop\esp32_newproject\sample_project

echo 1/3: Cleaning old build...
if exist build rmdir /s /q build
mkdir build

echo 2/3: Configuring cmake...
cmake -G Ninja -DCMAKE_MAKE_PROGRAM=%CMAKE_MAKE_PROGRAM% -DPYTHON_DEPS_CHECKED=1 -DPYTHON=D:\esp32_s3\idf\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe -DESP_PLATFORM=1 -DCCACHE_ENABLE=0 -B build
if %ERRORLEVEL% NEQ 0 echo CONFIGURE FAILED && exit /b 1

echo 3/3: Building...
ninja -C build
if %ERRORLEVEL% NEQ 0 echo BUILD FAILED && exit /b 1

echo ===== BUILD SUCCESS =====
echo Now flash with: idf.py -p COM5 flash
