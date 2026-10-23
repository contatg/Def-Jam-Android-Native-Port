cmake_minimum_required(VERSION 3.20)
project(guest_cpu_posix_test C)
# One guest CPU without thread affinity (win32_compat.c): threads that have
# joined never run at the same instant, one that spins is preempted for one
# that is waiting, and a blocking wait gives the CPU up.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX guest-CPU token")
endif()
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)
add_executable(guest_cpu_posix_test test_main.c
    ../../src/platform/win32_compat.c ../../src/platform/mmio_trap.c)
target_include_directories(guest_cpu_posix_test PRIVATE ../../src/platform)
target_compile_options(guest_cpu_posix_test PRIVATE -Wall -Wextra -O2)
target_link_libraries(guest_cpu_posix_test PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
enable_testing()
add_test(NAME guest_cpu_posix_test COMMAND guest_cpu_posix_test)
