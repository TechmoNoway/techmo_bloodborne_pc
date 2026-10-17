@echo off
setlocal
set "techmo_gc_mode=1"
if /I "%~1"=="off" set "techmo_gc_mode=0"
if not "%~1"=="" if /I not "%~1"=="on" if /I not "%~1"=="off" (
    echo Usage: test-vram-writeback.cmd [on^|off]
    exit /b 2
)
pushd "%~dp0..\.."
if not exist "out\bb-probe.exe" (
    echo Build the native engine before running this experiment.
    popd
    exit /b 1
)
set "BB_IMAGE_WRITEBACK_GC=%techmo_gc_mode%"
set "BB_WRITE_TRACKING=1"
set "BB_FRAME_STATS=1"
set "BB_PREBUILT=1"
echo Techmo tiled VRAM writeback experiment: %techmo_gc_mode%
echo Using saved launcher settings. Both modes enable CPU write tracking and frame logs.
echo Compare the same route with off, then on. Normal launcher runs leave this experiment off.
call run.bat
set "techmo_test_exit=%ERRORLEVEL%"
popd
exit /b %techmo_test_exit%
