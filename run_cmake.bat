@echo off
set PATH=D:\esp32_s3\idf\Espressif\tools\xtensa-esp32s3-elf\esp-12.2.0_20230208\xtensa-esp32s3-elf\bin;D:\esp32_s3\idf\Espressif\tools\ninja\1.10.2;%PATH%
cd /d C:\Users\Lenovo\Desktop\esp32_newproject\sample_project
echo Running cmake...
cmake -G Ninja -DCMAKE_MAKE_PROGRAM=D:\esp32_s3\idf\Espressif\tools\ninja\1.10.2\ninja.exe -DPYTHON_DEPS_CHECKED=1 -DPYTHON=D:\esp32_s3\idf\Espressif\python_env\idf5.1_py3.11_env\Scripts\python.exe -DESP_PLATFORM=1 -DCCACHE_ENABLE=0 -B build 2>&1
echo EXIT_CODE=%ERRORLEVEL%
