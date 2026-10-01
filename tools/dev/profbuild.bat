@echo off
call D:\NFSMW\NFSMW-Recompiled_ff\tools\_entorno_vs.bat >nul
cd /d D:\NFSMW\rexglue-sdk
cmake -S . -B out/build/win-amd64-prof -G "Ninja Multi-Config" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_C_FLAGS=-march=x86-64-v2" "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" -DCMAKE_CXX_STANDARD=23 "-DCMAKE_CONFIGURATION_TYPES=Release" "-DCMAKE_C_FLAGS_RELEASE=-O3 -DNDEBUG -g" "-DCMAKE_CXX_FLAGS_RELEASE=-O3 -DNDEBUG -g" "-DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-g" "-DCMAKE_EXE_LINKER_FLAGS_RELEASE=-g" -DREXGLUE_USE_VULKAN=ON > "%TEMP%\claude\prof_configure.log" 2>&1
echo CONFIGURE_EXIT %ERRORLEVEL%
cmake --build out/build/win-amd64-prof --config Release --target rexgpu-xenos rexruntime > "%TEMP%\claude\prof_build.log" 2>&1
echo BUILD_EXIT %ERRORLEVEL%
