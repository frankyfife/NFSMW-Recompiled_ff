@echo off
call D:\NFSMW\NFSMW-Recompiled_ff\tools\_entorno_vs.bat >nul
cd /d D:\NFSMW\NFSMW-Recompiled_ff\tools\cpuprof
clang++ -O2 -std=c++20 sampler.cpp -o %TEMP%\claude\sampler.exe -ldbghelp 2>&1
echo EXIT %ERRORLEVEL%
