add_library(plugin_api
    INTERFACE
        plugin_api.h
)

add_library(plugin::api ALIAS plugin_api)

target_include_directories(plugin_api
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)