# NFS Most Wanted Recompiled - Windows launcher (Qt 6 Widgets)
#
# Standalone project, built by launcher-windows\build.bat:
#   cmake -S launcher-windows -B launcher-windows/out -G Ninja
#         -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64
# Qt's MSVC binaries need the MSVC compiler (cl), not clang.

cmake_minimum_required(VERSION 3.25)
project(nfsmw_launcher_windows LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets)

add_executable(nfsmw_launcher WIN32
    src/main.cpp
    src/theme.cpp
    src/theme.h
    src/widgets.cpp
    src/widgets.h
    src/iso_extractor.cpp
    src/iso_extractor.h
    src/launcher_window.cpp
    src/launcher_window.h
    res/launcher.rc
)

target_link_libraries(nfsmw_launcher PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
target_compile_definitions(nfsmw_launcher PRIVATE
    NFSMW_LAUNCHER_VERSION="1.0.0"
    UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
if(MSVC)
    target_compile_options(nfsmw_launcher PRIVATE /W4 /permissive- /utf-8)
endif()

# The launcher takes the game's name in build\ so double-clicking the icon
# opens the options window (the game itself is nfsmw.exe).
set_target_properties(nfsmw_launcher PROPERTIES OUTPUT_NAME "NFS_Most_Wanted")
