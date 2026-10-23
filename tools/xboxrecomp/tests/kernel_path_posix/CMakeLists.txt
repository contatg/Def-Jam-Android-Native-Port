cmake_minimum_required(VERSION 3.20)
project(kernel_path_posix_test C)
# Guest paths on a case-sensitive POSIX file system. Xbox paths ignore case and
# titles rely on it; the disc's own spelling has to be found below the mapped
# root without ever touching the folders above it.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX path translator")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_path_posix_test test_main.c)
target_include_directories(kernel_path_posix_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_path_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_path_posix_test COMMAND kernel_path_posix_test)
