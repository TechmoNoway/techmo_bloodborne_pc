@echo off
setlocal
pushd "%~dp0..\.."
set "techmo_python=builds\_internal\toolchain\clang64\bin\python.exe"
if not exist "%techmo_python%" (
    echo Build the native toolchain first, or run tools\pipeline_replay.py with Python 3.
    popd
    exit /b 2
)
"%techmo_python%" -B tools\pipeline_replay.py %*
set "techmo_result=%ERRORLEVEL%"
popd
exit /b %techmo_result%
