

add_library(plugin_aspect INTERFACE chain.hpp)
add_library(plugin::aspect ALIAS plugin_aspect)

target_include_directories(plugin_aspect
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
