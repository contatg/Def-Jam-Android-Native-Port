cmake_minimum_required(VERSION 3.14)

if(NOT ANDROID OR NOT "${CMAKE_ANDROID_ARCH_ABI}" STREQUAL "arm64-v8a")
    message(FATAL_ERROR "libadrenotools is only built for Android arm64-v8a")
endif()

project(defjam_adrenotools LANGUAGES C CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_subdirectory(lib/linkernsbypass)

add_library(adrenotools STATIC
    src/driver.cpp
    include/adrenotools/driver.h
    include/adrenotools/priv.h)
target_include_directories(adrenotools PUBLIC include PRIVATE .)
target_compile_options(adrenotools PRIVATE -Wall -Wextra)
target_link_libraries(adrenotools PUBLIC android dl PRIVATE linkernsbypass log)

add_subdirectory(src/hook)

