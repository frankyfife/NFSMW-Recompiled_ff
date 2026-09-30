@echo off
rem ===========================================================================
rem  Records the frame times of nfsmw.exe with PresentMon (ships with NVIDIA
rem  FrameView), independent of the game's own log.
rem
rem  PresentMon reads Windows' ETW events, which needs administrator rights:
rem  Windows asks once (UAC) when this starts.
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
echo Press F10 in the game to start and stop a recording. Close the game to end.
"%PM%" --process_name nfsmw.exe --output_file "%OUT%\frametimes.csv" --hotkey F10 ^
       --v1_metrics --qpc_time_ms --terminate_on_proc_exit --stop_existing_session ^
       --restart_as_admin
endlocal
