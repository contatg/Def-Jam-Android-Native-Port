cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.9.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Guest memory sizes, fixed per build (kernel.h). The contiguous window is the
# console's 64 MB unless a port asks for more; MmQueryStatistics reports 64 MB
# of RAM unless a port asks for more. A title that sizes its arena from the free
# pages and takes it from the window needs both raised together.
set(XBOXRECOMP_CONTIG_MB 64 CACHE STRING "Size of the contiguous window in MB (64 = the console)")
set(XBOXRECOMP_RAM_REPORT_MB 64 CACHE STRING "Physical memory MmQueryStatistics reports, in MB (64 = the console)")
if(XBOXRECOMP_CONTIG_MB LESS 64 OR XBOXRECOMP_CONTIG_MB GREATER 192)
    message(FATAL_ERROR "XBOXRECOMP_CONTIG_MB must be 64..192 (window + 64 MB of RAM within 256 MB)")
endif()
add_compile_definitions(XBOX_CONTIG_MB=${XBOXRECOMP_CONTIG_MB}
                        XBOX_REPORTED_RAM_MB=${XBOXRECOMP_RAM_REPORT_MB})

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/settings)
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: D3D8 → D3D11 compatibility layer
add_subdirectory(src/d3d)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_compile_definitions(xboxrecomp INTERFACE XBOX_CONTIG_MB=${XBOXRECOMP_CONTIG_MB}
                           XBOX_REPORTED_RAM_MB=${XBOXRECOMP_RAM_REPORT_MB})
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
)

