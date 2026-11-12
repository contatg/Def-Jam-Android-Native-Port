cmake_minimum_required(VERSION 3.14)

add_library(defjam_volk STATIC volk.c)
target_include_directories(defjam_volk PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_definitions(defjam_volk PUBLIC VK_NO_PROTOTYPES VK_USE_PLATFORM_ANDROID_KHR)
set_target_properties(defjam_volk PROPERTIES POSITION_INDEPENDENT_CODE ON)

