# MIT backend only. No proprietary DLL or extracted shader is bundled.
cmake_minimum_required(VERSION 3.20)
set(lsfg27_root "${CMAKE_CURRENT_LIST_DIR}")
file(GLOB lsfg27_core CONFIGURE_DEPENDS
    "${lsfg27_root}/vendor/framegen/src/core/*.cpp"
    "${lsfg27_root}/vendor/framegen/src/common/*.cpp"
    "${lsfg27_root}/vendor/framegen/src/pool/*.cpp"
    "${lsfg27_root}/vendor/framegen/v3.1p_src/shaders/*.cpp")
add_library(defjam_lsfg27 SHARED ${lsfg27_core}
    borrowed27.cpp image27.cpp vendor/volk/volk.c)
target_compile_features(defjam_lsfg27 PRIVATE cxx_std_20)
target_include_directories(defjam_lsfg27 PRIVATE
    "${lsfg27_root}" "${lsfg27_root}/vendor/volk"
    "${lsfg27_root}/vendor/framegen/include"
    "${lsfg27_root}/vendor/framegen/v3.1p_include")
set_target_properties(defjam_lsfg27 PROPERTIES
    C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)
target_compile_options(defjam_lsfg27 PRIVATE -O2 -ffunction-sections -fdata-sections)
# Bind the private Volk globals locally even when the game also uses Volk.
target_link_options(defjam_lsfg27 PRIVATE -Wl,-Bsymbolic -Wl,--gc-sections
    "-Wl,--version-script=${lsfg27_root}/exports.map")
if(ANDROID)
    target_link_libraries(defjam_lsfg27 PRIVATE android log dl)
else()
    target_link_libraries(defjam_lsfg27 PRIVATE dl)
endif()
