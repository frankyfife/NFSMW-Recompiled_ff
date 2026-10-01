# nfsmw - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nfsmw LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NFSMW_SOURCES
    src/main.cpp
    src/nfsmw_menu.cpp
    src/render_census.cpp
    src/render_capture.cpp
    src/crash_log.cpp
    src/freecam.cpp
    src/post_processing.cpp
    src/car_lod.cpp
)

if(WIN32)
    add_executable(nfsmw WIN32 ${NFSMW_SOURCES})
else()
    add_executable(nfsmw ${NFSMW_SOURCES})
endif()

# Native renderer running next to the emulation (--native_renderer, see
# docs/NATIVE_RENDERER.md): Direct3D 12, with the SDK's Xenos shader
# translator compiled in from the rexglue-sdk source tree (the GPU plugin does
# not export it). Its cvars stay local (src/native/sdk_cvars_local.h) so the
# plugin's own settings keep working.
set(REXSDK_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../rexglue-sdk" CACHE PATH
    "rexglue-sdk source tree, for the native renderer")
option(NFSMW_NATIVE_RENDERER "Build the native renderer (Windows)" ON)
if(WIN32 AND NFSMW_NATIVE_RENDERER AND
   EXISTS "${REXSDK_SOURCE_DIR}/src/graphics/pipeline/shader/dxbc_translator.cpp")
    set(SDK_GPU "${REXSDK_SOURCE_DIR}/src/graphics")
    set(NATIVE_SDK_SOURCES
        ${SDK_GPU}/flags.cpp
        ${SDK_GPU}/registers.cpp
        ${SDK_GPU}/register_file.cpp
        ${SDK_GPU}/format/ucode.cpp
        ${SDK_GPU}/util/draw.cpp
        ${SDK_GPU}/pipeline/shader/shader.cpp
        ${SDK_GPU}/pipeline/shader/translator.cpp
        ${SDK_GPU}/pipeline/shader/translator_disasm.cpp
        ${SDK_GPU}/pipeline/shader/dxbc.cpp
        ${SDK_GPU}/pipeline/shader/dxbc_translator.cpp
        ${SDK_GPU}/pipeline/shader/dxbc_translator_alu.cpp
        ${SDK_GPU}/pipeline/shader/dxbc_translator_fetch.cpp
        ${SDK_GPU}/pipeline/shader/dxbc_translator_memexport.cpp
        ${SDK_GPU}/pipeline/shader/dxbc_translator_om.cpp
    )
    target_sources(nfsmw PRIVATE
        src/native/parallel.cpp
        src/native/renderer.cpp
        ${NATIVE_SDK_SOURCES}
        ${REXSDK_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
    )
    set_source_files_properties(${NATIVE_SDK_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/native/sdk_cvars_local.h;-Wno-unused-variable"
        SKIP_PRECOMPILE_HEADERS ON)
    # Line tables for the renderer (profiling with tools/cpuprof: where in
    # Draw the time goes); the code is the same.
    set_source_files_properties(src/native/parallel.cpp src/native/renderer.cpp PROPERTIES
        COMPILE_OPTIONS "-gline-tables-only")
    set_source_files_properties(${REXSDK_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp PROPERTIES
        COMPILE_OPTIONS "-w" SKIP_PRECOMPILE_HEADERS ON)
    target_include_directories(nfsmw PRIVATE
        src/native ${REXSDK_SOURCE_DIR} ${REXSDK_SOURCE_DIR}/thirdparty/renderdoc)
    target_link_libraries(nfsmw PRIVATE d3d12 dxgi dxguid d3dcompiler windowscodecs ole32)
    message(STATUS "Native renderer: on (SDK source ${REXSDK_SOURCE_DIR})")
else()
    target_sources(nfsmw PRIVATE src/native/parallel_stub.cpp)
    message(STATUS "Native renderer: off")
endif()

# Public symbols (sub_XXXXXXXX) in a PDB for the sampling profiler, link only.
if(WIN32)
    target_link_options(nfsmw PRIVATE "LINKER:/DEBUG" "LINKER:/OPT:REF" "LINKER:/OPT:ICF")
endif()

# GPU_PLUGINS deja el DLL del backend grafico junto al .exe
# (rexgpu-xenosd.dll en Debug, rexgpu-xenos.dll en Release).
#
# Hacen falta LAS DOS COSAS para ver imagen:
#   1. esta linea, que coloca el DLL donde el cargador lo busca
#   2. que gpu_plugin valga "xenos" al ejecutar
#
# El CVar gpu_plugin viene vacio de fabrica, y vacio significa "sin emulacion
# de GPU": el juego inicializa su ring buffer y sus callbacks graficos, el
# runtime los descarta con "no GPU emulation loaded", y la pantalla se queda
# en negro aunque el juego este corriendo perfectamente.
#
# Desde que src/nfsmw_app.h pone gpu_plugin=xenos por defecto, el argumento
# --gpu_plugin ya no hace falta; se puede seguir pasando para cambiarlo.
rexglue_setup_target(nfsmw GPU_PLUGINS xenos)

# Native Qt launcher (Linux). Used by packaging/appimage/.
option(NFSMW_BUILD_LAUNCHER "Build the native Qt launcher" ON)
option(NFSMW_LAUNCHER_BUILD_TESTS "Build Qt launcher tests" OFF)
if(NFSMW_BUILD_LAUNCHER AND UNIX AND NOT APPLE)
    find_package(Qt6 6.4 COMPONENTS Core Gui Widgets QUIET)
    if(Qt6_FOUND)
        if(NFSMW_LAUNCHER_BUILD_TESTS)
            enable_testing()
        endif()
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../launcher-linux"
                         "${CMAKE_CURRENT_BINARY_DIR}/launcher-linux")
    else()
        message(WARNING
            "Qt 6.4+ was not found; nfsmw_launcher will not be built")
    endif()
endif()


# =============================================================================
#  TARGET "dist" - la carpeta portable, que se llama  build\
#
#  Junta en una sola carpeta el ejecutable, las DLL del proyecto y las del
#  runtime de Visual C++, para poder llevarsela a un equipo sin herramientas
#  de desarrollo.
#
#      cmake --build --preset win-amd64-release --target dist
#
#  EL TARGET SE LLAMA "dist" Y LA CARPETA "build". No es un descuido: build/
#  ya estaba en el .gitignore y dist/ no, asi que dejando ahi la carpeta -que
#  lleva dentro el ejecutable con el codigo del juego- queda fuera de git
#  sola. Para ponerla en otro sitio:  -D NFSMW_DIST_DIR=...
#
#  NO CONFUNDIR con app/out/build/win-amd64-release, que es donde compila
#  CMake. Esta es la carpeta ya montada, lista para llevarsela.
#
#  LO QUE NO HACE, Y ES IMPORTANTE:
#  no mete la ISO dentro de nada. Sigue siendo un archivo suelto que el juego
#  lee al vuelo, y src/nfsmw_app.h la busca junto al .exe. Asi la carpeta
#  funciona este donde este, sin ninguna ruta fija.
#
#  Nada de esto cambia como se construye el proyecto: es un paso mas, aparte.
# =============================================================================
if(WIN32)
    # Resuelve las DLL del runtime de VC desde el toolchain que se este usando.
    # Con _SKIP a TRUE no instala nada: solo rellena la lista, que es lo unico
    # que queremos. Sin esto habria que escribir a mano una ruta tipo
    # "C:\Program Files\Microsoft Visual Studio\...\Microsoft.VC143.CRT", que es
    # exactamente lo que no debe haber aqui.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_NO_WARNINGS TRUE)
    include(InstallRequiredSystemLibraries)

    # SIN cache a proposito. Si fueran variables de cache, cambiar el valor
    # por defecto no serviria de nada en un arbol ya configurado: seguiria
    # usando el viejo sin decir ni pio, el target diria "hecho" y la carpeta
    # apareceria donde antes. Asi manda siempre lo de aqui, y un -D de la
    # linea de comandos lo sigue pisando.
    if(NOT DEFINED NFSMW_DIST_NOMBRE)
        set(NFSMW_DIST_NOMBRE "NFS_Most_Wanted")
    endif()
    if(NOT DEFINED NFSMW_DIST_DIR)
        set(NFSMW_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build")
    endif()

    add_custom_target(dist
        DEPENDS nfsmw
        COMMAND ${CMAKE_COMMAND}
                -D "D_EXE=$<TARGET_FILE:nfsmw>"
                -D "D_BUILD=$<TARGET_FILE_DIR:nfsmw>"
                -D "D_DIST=${NFSMW_DIST_DIR}"
                -D "D_NOMBRE=${NFSMW_DIST_NOMBRE}"
                -D "D_VCRUNTIME=${CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS}"
                -D "D_REDIST=$ENV{VCToolsRedistDir}"
                -D "D_PLANTILLA=${CMAKE_CURRENT_SOURCE_DIR}/dist_leeme.txt"
                -D "D_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/nfsmw.toml"
                -D "D_EDRAM=${CMAKE_CURRENT_SOURCE_DIR}/dist_comparar_edram.bat"
                -D "D_LANZADOR=${CMAKE_CURRENT_SOURCE_DIR}/../tools/lanzador/lanzador.ps1"
                -D "D_LANZADOR_BAT=${CMAKE_CURRENT_SOURCE_DIR}/dist_lanzador.bat"
                -D "D_MATRIZ=${CMAKE_CURRENT_SOURCE_DIR}/../tools/matriz.ps1"
                -D "D_PROBAR=${CMAKE_CURRENT_SOURCE_DIR}/dist_probar.bat"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hacer_dist.cmake"
        COMMENT "Armando la carpeta portable en ${NFSMW_DIST_DIR}"
        VERBATIM
    )
endif()
