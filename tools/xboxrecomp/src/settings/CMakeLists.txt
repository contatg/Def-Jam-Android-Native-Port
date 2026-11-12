# Player-facing settings: a typed table backed by one INI file.
# Portable C, no dependencies.

add_library(recomp_settings STATIC recomp_settings.c)
target_include_directories(recomp_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(recomp_settings PRIVATE /W4)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(recomp_settings PUBLIC Threads::Threads)
    target_compile_options(recomp_settings PRIVATE -Wall -Wextra)
endif()
