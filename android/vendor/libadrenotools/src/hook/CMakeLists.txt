add_library(hook_impl SHARED hook_impl.cpp hook_impl.h hook_impl_params.h kgsl.h)
target_compile_options(hook_impl PRIVATE -Wall -Wextra)
target_link_libraries(hook_impl PRIVATE linkernsbypass log android dl)
target_include_directories(hook_impl PRIVATE ../../include)
set_target_properties(hook_impl PROPERTIES CXX_VISIBILITY_PRESET hidden)

add_library(main_hook SHARED main_hook.c)
target_compile_options(main_hook PRIVATE -Wall -Wextra)
target_link_options(main_hook PRIVATE "-Wl,-z,global")
target_link_libraries(main_hook PRIVATE hook_impl)
set_target_properties(main_hook PROPERTIES C_VISIBILITY_PRESET hidden)

