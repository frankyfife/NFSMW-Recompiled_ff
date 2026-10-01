@echo off
call D:\NFSMW\NFSMW-Recompiled_ff\tools\_entorno_vs.bat >nul
cd /d D:\NFSMW\NFSMW-Recompiled_ff\app
cmake --build --preset win-amd64-release 2>&1 | findstr /i /c:"error" /c:"FAILED" /c:"Linking"
echo GAME_EXIT %ERRORLEVEL%
