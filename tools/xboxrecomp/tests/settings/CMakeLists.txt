cmake_minimum_required(VERSION 3.20)
project(recomp_settings_test C)
set(CMAKE_C_STANDARD 11)
# The settings library alone: it has no dependency on the rest of the runtime.
add_subdirectory(../../src/settings settings)
add_executable(recomp_settings_test test_main.c)
target_link_libraries(recomp_settings_test PRIVATE recomp_settings)
enable_testing()
add_test(NAME recomp_settings_test COMMAND recomp_settings_test)
