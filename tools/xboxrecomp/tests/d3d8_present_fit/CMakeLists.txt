cmake_minimum_required(VERSION 3.20)
project(d3d8_present_fit_test C)
set(CMAKE_C_STANDARD 11)
# Header-only arithmetic: no Direct3D device and no runtime library.
add_executable(d3d8_present_fit_test test_main.c)
target_include_directories(d3d8_present_fit_test PRIVATE ../../src/d3d)
enable_testing()
add_test(NAME d3d8_present_fit_test COMMAND d3d8_present_fit_test)
