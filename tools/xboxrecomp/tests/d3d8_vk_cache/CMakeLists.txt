cmake_minimum_required(VERSION 3.20)
project(d3d8_vk_cache_test C)
set(CMAKE_C_STANDARD 11)
# The Vulkan device's on-disk shader and pipeline cache: file formats only, so
# no Vulkan driver or shaderc is needed.
add_executable(d3d8_vk_cache_test test_main.c ../../src/d3d/d3d8_vk_cache.c)
target_include_directories(d3d8_vk_cache_test PRIVATE ../../src/d3d)
enable_testing()
add_test(NAME d3d8_vk_cache_test COMMAND d3d8_vk_cache_test)
