# SDL3, fetched and built statically for gamepad input.
#
#   include(xbox_sdl3)   # defines the target SDL3::SDL3-static
#
# Opt-in: nothing includes this unless XBOXRECOMP_SDL3 is on, so the toolkit's
# default build has no dependency on it and needs no network. The release is
# pinned by URL and hash; it is zlib licensed (see LICENSES/SDL3-zlib.txt and
# NOTICE). Only the joystick and gamepad subsystems are used, so the rest of
# SDL is switched off to keep the build small.
include_guard(GLOBAL)
include(FetchContent)

set(XBOX_SDL3_VERSION "3.4.18")
set(XBOX_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_GPU OFF CACHE BOOL "" FORCE)
set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
set(SDL_DIALOG OFF CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)

FetchContent_Declare(sdl3
    URL https://github.com/libsdl-org/SDL/releases/download/release-${XBOX_SDL3_VERSION}/SDL3-${XBOX_SDL3_VERSION}.tar.gz
    URL_HASH SHA256=${XBOX_SDL3_SHA256})
FetchContent_MakeAvailable(sdl3)
