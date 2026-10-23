cmake_minimum_required(VERSION 3.20)
project(d3d8_readback_fixture C)
set(CMAKE_C_STANDARD 11)
# The asynchronous back-buffer read-back against LockRect, on this platform's
# real Direct3D 8 device. Needs a Direct3D 11 hardware device (the Basic Render Driver of a
# machine without a GPU will do) on Windows,
# a Vulkan driver (lavapipe will do) elsewhere; with neither ctest reports it skipped.
add_subdirectory(../../src/platform platform)
add_subdirectory(../../src/d3d d3d)
add_executable(d3d8_readback_test test_main.c)
target_link_libraries(d3d8_readback_test PRIVATE xbox_d3d8)
if(NOT WIN32)
    target_link_libraries(platform PUBLIC m)    # its fenv calls; after it on the link line
endif()
enable_testing()
add_test(NAME d3d8_readback COMMAND d3d8_readback_test)
add_test(NAME d3d8_readback_scaled COMMAND d3d8_readback_test scaled)
set_tests_properties(d3d8_readback d3d8_readback_scaled PROPERTIES SKIP_RETURN_CODE 77)
