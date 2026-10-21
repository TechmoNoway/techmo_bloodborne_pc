@echo off
setlocal
set "PYTHONUTF8=1"
set "PYTHONIOENCODING=utf-8"
set "PYTHONUNBUFFERED=1"
set "TECHMO_PYTHON=%~dp0builds\_internal\toolchain\clang64\bin\python.exe"
if exist "%TECHMO_PYTHON%" goto run
set "TECHMO_PYTHON=%~dp0..\techmo_launcher\builds\_internal\toolchain\clang64\bin\python.exe"
if not exist "%TECHMO_PYTHON%" (
    echo Python runtime missing. Set up the toolchain before starting experiment-v2.
    exit /b 1
)
:run
"%TECHMO_PYTHON%" -B "%~dp0scripts\run_lantern.py" %*
exit /b %ERRORLEVEL%
