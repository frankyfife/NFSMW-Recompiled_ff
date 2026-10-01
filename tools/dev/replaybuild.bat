@echo off
rem Builds tools/replay (nfsmw_replay.exe) against the installed ReXGlue SDK.
call D:\NFSMW\NFSMW-Recompiled_ff\tools\_entorno_vs.bat >nul
cd /d D:\NFSMW\NFSMW-Recompiled_ff\tools\replay
if not exist out\build.ninja (
  cmake -S . -B out -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_PREFIX_PATH=D:/NFSMW/rexglue-sdk/out/install/win-amd64 > "%TEMP%\claude\replay_configure.log" 2>&1
  echo CONFIGURE_EXIT %ERRORLEVEL%
)
cmake --build out > "%TEMP%\claude\replay_build.log" 2>&1
echo BUILD_EXIT %ERRORLEVEL%
