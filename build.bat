@echo off
setlocal
REM ESP-IDF build helper - sets environment and builds the project

set IDF_TOOLS_PATH=D:\esp32_s3\idf\Espressif
set IDF_PATH=%IDF_TOOLS_PATH%\frameworks\esp-idf-v5.1.2
set IDF_PYTHON_ENV_PATH=%IDF_TOOLS_PATH%\python_env\idf5.1_py3.11_env
set PYTHON=%IDF_PYTHON_ENV_PATH%\Scripts\python.exe
set PATH=%IDF_TOOLS_PATH%\tools\cmake\3.24.0\bin;%IDF_TOOLS_PATH%\tools\ninja\1.10.2;%IDF_TOOLS_PATH%\tools\idf-git\2.43.0\cmd;%IDF_TOOLS_PATH%\tools\xtensa-esp32s3-elf\esp-12.2.0_20230208\xtensa-esp32s3-elf\bin;%IDF_PYTHON_ENV_PATH%\Scripts;%PATH%
set IDF_TARGET=esp32s3

echo === Building ===
"%PYTHON%" "%IDF_PATH%\tools\idf.py" build
if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)

echo === Build complete ===
