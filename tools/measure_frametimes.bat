@echo off
rem ===========================================================================
rem  Records the frame times of nfsmw.exe with PresentMon (ships with NVIDIA
rem  FrameView), independent of the game's own log.
rem
rem  PresentMon reads Windows' ETW events, which needs administrator rights.
rem  This script restarts itself elevated (Windows asks once, UAC). PresentMon's
rem  own --restart_as_admin did nothing in the FrameView build: it just exits.
rem
rem  1. Start this script, then the game (or the other way round).
rem  2. In the game press F10 to start recording, F10 again to stop.
rem     Several recordings in one session are fine.
rem  3. Close the game: PresentMon ends with it.
rem  4. python tools\analyze_frametimes.py   (writes an HTML report)
rem
rem  The CSV files land in build\logs\frametimes\.
rem ===========================================================================
setlocal

rem Elevated? "net session" only works as administrator.
net session >nul 2>&1
if errorlevel 1 (
    echo Asking Windows for administrator rights...
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "PM=%ProgramFiles%\NVIDIA Corporation\FrameViewSDK\bin\PresentMon_x64.exe"
if not exist "%PM%" (
    echo PresentMon not found at:
    echo   %PM%
    echo Install NVIDIA FrameView, or get PresentMon from
    echo   https://github.com/GameTechDev/PresentMon/releases
    echo and fix the PM line in this script.
    pause
    exit /b 1
)

rem Next to nfsmw.exe (copied into build\) or in tools\ of the repository.
if exist "%~dp0nfsmw.exe" (
    set "OUT=%~dp0logs\frametimes"
) else (
    set "OUT=%~dp0..\build\logs\frametimes"
)
if not exist "%OUT%" mkdir "%OUT%"

echo Recording into %OUT%
echo.
echo Press F10 in the game to start a recording, F10 again to stop it.
echo Close the game (or this window) when you are done.
echo.
"%PM%" --process_name nfsmw.exe --output_file "%OUT%\frametimes.csv" --hotkey F10 ^
       --v1_metrics --qpc_time_ms --stop_existing_session
echo.
echo PresentMon ended (exit code %errorlevel%). Files in %OUT%:
dir /b "%OUT%\*.csv" 2>nul
pause
endlocal
