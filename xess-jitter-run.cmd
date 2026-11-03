@echo off
setlocal EnableExtensions DisableDelayedExpansion

set "GAME_DIR=%~dp0build\kz3-win"
if defined KZ3_XESS_BUILD_DIR for %%I in ("%KZ3_XESS_BUILD_DIR%") do set "GAME_DIR=%%~fI"
set "GAME_EXE=%GAME_DIR%\kz3.exe"

if not exist "%GAME_EXE%" (
    echo ERROR: Missing current-branch executable:
    echo   %GAME_EXE%
    exit /b 2
)

set "KZ3_DISPLAY=windowed"
set "KZ3_WINDOW_SIZE=1920x1080"
set "KZ3_DISPLAY_SCALING=fit"
set "KZ3_DISPLAY_FILTER=xess"
set "KZ3_FPS_LIMIT=30"
set "KZ3_LOG_STDOUT=1"
set "KZ3_XESS_MIP_BIAS="
set "KZ3_XESS_CAMERA_LAG="
set "KZ3_XESS_HUD_MASK=1"

cd /d "%GAME_DIR%"
echo Starting the current XeSS build. Enter gameplay, pause on a still scene,
echo and check whether the whole picture continues to move.
"%GAME_EXE%"
exit /b %ERRORLEVEL%
