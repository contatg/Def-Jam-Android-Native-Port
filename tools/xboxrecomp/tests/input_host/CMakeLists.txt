cmake_minimum_required(VERSION 3.20)
project(input_host_test C)
set(CMAKE_C_STANDARD 11)
# The host input layer against SDL3's virtual gamepads: no pad on the desk.
# Windows only (the layer reads XInput and sits on the Win32 API).
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake)
include(xbox_sdl3)
add_executable(input_host_test test_main.c
    ../../src/input/input_host.c ../../src/input/input_map.c)
target_include_directories(input_host_test PRIVATE ../../src/input ../../src)
target_compile_definitions(input_host_test PRIVATE XBOXRECOMP_HAVE_SDL3=1)
target_link_libraries(input_host_test PRIVATE SDL3::SDL3-static xinput winmm)
enable_testing()
add_test(NAME input_host_test COMMAND input_host_test)
