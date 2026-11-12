# Deferred from the root directory: targets already exist, including builds
# whose Android branch returns before the desktop CMake code.
if(ANDROID)
    # Backend is created immediately after project(). Deferred execution may
    # update target links but may not create another subdirectory.
    if(TARGET xbox_d3d8)
        target_link_libraries(xbox_d3d8 PUBLIC defjam_lsfg27)
    elseif(TARGET main)
        target_link_libraries(main PRIVATE defjam_lsfg27)
    else()
        message(FATAL_ERROR "LSFG27: expected Android target main or xbox_d3d8 is absent")
    endif()
endif()
