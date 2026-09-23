cmake_minimum_required(VERSION 3.20)
project(floidsense LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(floidsense WIN32   # WinMain entry, no console window
    src/main.cpp
    src/overlay.cpp
    src/launcher.cpp
)

# src/ for local headers, output/ for offsets.hpp / client_dll.hpp etc.
target_include_directories(floidsense PRIVATE src output)

target_compile_options(floidsense PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /MP /utf-8>
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra>
)

# All Win32 libs are in the default MSVC link line — listed explicitly
# so MinGW builds also link correctly.
target_link_libraries(floidsense PRIVATE user32 gdi32 kernel32 winmm)
