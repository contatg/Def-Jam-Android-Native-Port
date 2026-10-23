# Xbox Input → XInput Compatibility Layer, and the host-side input rules

# Read pads through SDL3 (DualSense, Switch Pro, DirectInput and XInput under
# one set of names). Off by default: the build then needs no extra dependency
# and pads are read through XInput.
option(XBOXRECOMP_SDL3 "Read gamepads through SDL3 (fetched and built statically)" OFF)

add_library(xbox_input STATIC
    xinput_device.c
    input_map.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(WIN32)
    target_sources(xbox_input PRIVATE input_host.c)
    target_link_libraries(xbox_input PUBLIC platform xinput winmm)
    if(XBOXRECOMP_SDL3)
        list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake)
        include(xbox_sdl3)
        target_link_libraries(xbox_input PUBLIC SDL3::SDL3-static)
        target_compile_definitions(xbox_input PUBLIC XBOXRECOMP_HAVE_SDL3=1)
    endif()
elseif(XBOXRECOMP_SDL3)
    # Off Windows SDL3 is the system's, shared: the program's window and
    # sound use it as well, and a process cannot hold SDL2 and SDL3 at once
    # (they export the same names). Pads then go through the host input layer.
    if(NOT TARGET SDL3::SDL3)
        find_package(SDL3 REQUIRED CONFIG)
    endif()
    target_sources(xbox_input PRIVATE input_host.c)
    target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3)
    target_compile_definitions(xbox_input PUBLIC XBOXRECOMP_HAVE_SDL3=1)
else()
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_input PUBLIC platform SDL2::SDL2)
endif()

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
    target_link_libraries(xbox_input PUBLIC m)
endif()
