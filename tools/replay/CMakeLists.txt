# nfsmw_replay: draws a captured frame (render_capture_frame) natively with
# Direct3D 12, without the game and without EDRAM emulation. Stage 3 of the
# native renderer, see docs/NATIVE_RENDERER.md.
#
# Uses the installed ReXGlue SDK (rexruntime) and compiles the SDK's Xenos
# shader translator from its source tree (the GPU plugin does not export it).
#
#   tools\dev\replaybuild.bat
cmake_minimum_required(VERSION 3.25)
project(nfsmw_replay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(REXSDK_SOURCE_DIR "D:/NFSMW/rexglue-sdk" CACHE PATH "rexglue-sdk source tree")
find_package(rexglue 0.10.0 REQUIRED CONFIG)

set(SDK_GPU "${REXSDK_SOURCE_DIR}/src/graphics")
add_executable(nfsmw_replay
    replay.cpp
    capture.cpp
    renderer.cpp
    # Xenos shader translation from the SDK.
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
    ${REXSDK_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
)
target_include_directories(nfsmw_replay PRIVATE ${REXSDK_SOURCE_DIR} ${REXSDK_SOURCE_DIR}/thirdparty/renderdoc)
set_source_files_properties(${REXSDK_SOURCE_DIR}/thirdparty/dxbc/DXBCChecksum.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
target_compile_options(nfsmw_replay PRIVATE -march=x86-64-v2 -Wno-unused-variable)
target_link_libraries(nfsmw_replay PRIVATE rex::runtime rex::xxhash d3d12 dxgi dxguid d3dcompiler windowscodecs ole32)
