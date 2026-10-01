@echo off
call D:\NFSMW\NFSMW-Recompiled_ff\tools\_entorno_vs.bat >nul
cd /d D:\NFSMW\rexglue-sdk
cmake --build out/build/win-amd64 --config Release --target install 2>&1 | findstr /i /c:"error" /c:"FAILED" /c:"Linking" /c:"Install"
echo SDK_EXIT %ERRORLEVEL%
cd /d D:\NFSMW\NFSMW-Recompiled_ff
cmake --build --preset win-amd64-release 2>&1 | findstr /i /c:"error" /c:"FAILED" /c:"Linking"
echo GAME_EXIT %ERRORLEVEL%
