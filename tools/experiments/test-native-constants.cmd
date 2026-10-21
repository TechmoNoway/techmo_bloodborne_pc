@echo off
setlocal
set "BB_NATIVE_CONSTANTS=1"
if /I "%~1"=="off" set "BB_NATIVE_CONSTANTS=0"
if not "%~1"=="" if /I not "%~1"=="on" if /I not "%~1"=="off" (
    echo Usage: test-native-constants.cmd [on^|off]
    exit /b 2
)
pushd "%~dp0..\.."
if not exist "out\bb-probe.exe" (
    echo Build the native engine before running this experiment.
    popd
    exit /b 1
)
set "BB_FRAME_STATS=1"
set "BB_PREBUILT=1"
echo Native constants experiment: %BB_NATIVE_CONSTANTS%. Requires a rebuilt native engine.
echo Compare the same route and settings; each mode keeps its own pipeline cache.
call run.bat
set "techmo_result=%ERRORLEVEL%"
popd
exit /b %techmo_result%
