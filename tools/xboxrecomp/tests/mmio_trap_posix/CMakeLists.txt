cmake_minimum_required(VERSION 3.20)
project(mmio_trap_posix_test C)
# Trapped device registers off Windows: the A64 load/store decoder, and a
# 4 KB device page inside a larger host page (16 KB on Apple Silicon) whose
# neighbours must keep behaving as memory. Standalone: it compiles the one
# source file and needs no runtime library.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX trap path")
endif()
set(CMAKE_C_STANDARD 11)
add_executable(mmio_trap_posix_test test_main.c ../../src/platform/mmio_trap.c)
target_include_directories(mmio_trap_posix_test PRIVATE ../../src/platform)
target_compile_options(mmio_trap_posix_test PRIVATE -Wall -Wextra -O2)
enable_testing()
add_test(NAME mmio_trap_posix_test COMMAND mmio_trap_posix_test)
