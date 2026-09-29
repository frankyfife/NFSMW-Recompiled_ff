@echo off
rem ===========================================================================
rem  Prepara el entorno de compilacion. Se invoca con CALL desde los demas
rem  .bat, por eso NO lleva setlocal: las variables tienen que sobrevivir.
rem
rem  Deja listo:
rem    - el entorno x64 de Visual Studio (INCLUDE, LIB, PATH)
rem    - cmake / ninja / clang NATIVOS de VS por delante de cualquier MSYS2
rem    - el bin del ReXGlue SDK instalado, para que "rexglue" responda
rem    - ENTORNO_OK=1 si todo fue bien
rem ===========================================================================

set "ENTORNO_OK="
set "VSPATH="
set "PF=%ProgramFiles%"
set "PF86=%ProgramFiles(x86)%"

set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%PF%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VSPATH=%%i"
)

if defined VSPATH (
    if not exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH="
)

if not defined VSPATH (
    for %%R in ("%PF%\Microsoft Visual Studio" "%PF86%\Microsoft Visual Studio") do (
        if exist "%%~R\" (
            for /d %%Y in ("%%~R\*") do (
                if exist "%%~Y\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~Y"
                for /d %%E in ("%%~Y\*") do (
                    if exist "%%~E\VC\Auxiliary\Build\vcvars64.bat" set "VSPATH=%%~E"
                )
            )
        )
    )
)

if not defined VSPATH (
    echo [ERROR] No se encontro Visual Studio con herramientas de C++.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] vcvars64.bat fallo.
    exit /b 1
)

rem System32 PRIMERO, antes que las de VS. Con MSYS2 en el PATH, utilidades
rem de Windows como find.exe, sort.exe o where.exe quedan tapadas por sus
rem homonimas de Unix, que aceptan otros parametros y fallan de formas raras
rem ("find: /c/$Recycle.Bin: Permission denied"). Se antepone aqui para que
rem los prepends de VS que vienen despues queden por delante de esta.
set "PATH=%SystemRoot%\System32;%PATH%"

set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "VSNINJA=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja"
set "VSLLVM=%VSPATH%\VC\Tools\Llvm\x64\bin"
if exist "%VSNINJA%\ninja.exe" set "PATH=%VSNINJA%;%PATH%"
if exist "%VSCMAKE%\cmake.exe" set "PATH=%VSCMAKE%;%PATH%"
if exist "%VSLLVM%\clang.exe"  set "PATH=%VSLLVM%;%PATH%"

rem Sin el componente Clang de Visual Studio: el LLVM oficial en Program Files.
rem Si tampoco esta en el PATH, CMake no encuentra "clang" y la configuracion
rem del SDK falla en la primera linea.
if not exist "%VSLLVM%\clang.exe" (
    where clang >nul 2>nul
    if errorlevel 1 if exist "%PF%\LLVM\bin\clang.exe" set "PATH=%PF%\LLVM\bin;%PATH%"
)

rem El SDK instalado
set "SDK=%~dp0..\..\rexglue-sdk"
set "SDKBIN=%SDK%\out\install\win-amd64\bin"
if exist "%SDKBIN%" set "PATH=%SDKBIN%;%PATH%"

set "PY="
py -3 --version >nul 2>nul && set "PY=py -3"
if not defined PY (
    python --version >nul 2>nul && set "PY=python"
)

set "ENTORNO_OK=1"
exit /b 0
