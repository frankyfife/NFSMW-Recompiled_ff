@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  Builds the Qt launcher and puts it into build\ as NFS_Most_Wanted.exe,
rem  together with the Qt DLLs it needs (windeployqt).
rem
rem  Needs Qt 6 for MSVC 2022 x64. Looked up in this order:
rem    %QT_DIR%                      e.g. D:\Qt\6.8.3\msvc2022_64
rem    D:\Qt\*\msvc2022_64, C:\Qt\*\msvc2022_64   (newest folder wins)
rem  Install it with:  pip install aqtinstall
rem                    python -m aqt install-qt windows desktop 6.8.3 win64_msvc2022_64
rem                           --outputdir D:\Qt --archives qtbase
rem
rem  /silencioso: no pause at the end (used by CONSTRUIR.bat).
rem ===========================================================================

set "AQUI=%~dp0"
set "RAIZ=%AQUI%.."
set "SALIDA_BUILD=%RAIZ%\build"

if not defined QT_DIR (
    for %%B in (D C) do (
        for /d %%V in ("%%B:\Qt\6.*") do (
            if exist "%%~V\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QT_DIR=%%~V\msvc2022_64"
        )
    )
)
if not defined QT_DIR (
    echo [ERROR] Qt 6 for MSVC 2022 x64 not found. Set QT_DIR or see the header of this file.
    goto fin_mal
)
echo Qt: %QT_DIR%

call "%RAIZ%\tools\_entorno_vs.bat"
if not defined ENTORNO_OK goto fin_mal

cmake -S "%AQUI%." -B "%AQUI%out" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DCMAKE_CXX_COMPILER=cl -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 goto fin_mal
cmake --build "%AQUI%out"
if errorlevel 1 goto fin_mal

rem Fresh from the dist target the GAME is build\NFS_Most_Wanted.exe; it is
rem renamed to nfsmw.exe so the launcher can take the game's name.
if not exist "%SALIDA_BUILD%\nfsmw.exe" (
    if exist "%SALIDA_BUILD%\NFS_Most_Wanted.exe" (
        move /y "%SALIDA_BUILD%\NFS_Most_Wanted.exe" "%SALIDA_BUILD%\nfsmw.exe" >nul
        if errorlevel 1 (
            echo [ERROR] Could not rename the game to nfsmw.exe. Is it running?
            goto fin_mal
        )
    ) else (
        echo [aviso] build\ has no game yet; the launcher stays in launcher-windows\out.
        goto fin_bien
    )
)

copy /y "%AQUI%out\NFS_Most_Wanted.exe" "%SALIDA_BUILD%\NFS_Most_Wanted.exe" >nul
if errorlevel 1 (
    echo [ERROR] Could not copy the launcher. Is it open?
    goto fin_mal
)
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler ^
    --no-opengl-sw --no-compiler-runtime --no-quick-import --no-network ^
    "%SALIDA_BUILD%\NFS_Most_Wanted.exe"
if errorlevel 1 goto fin_mal

echo.
echo Launcher ready: build\NFS_Most_Wanted.exe

:fin_bien
if /i not "%~1"=="/silencioso" pause
exit /b 0

:fin_mal
if /i not "%~1"=="/silencioso" pause
exit /b 1
