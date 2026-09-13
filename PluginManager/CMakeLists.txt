
add_subdirectory(plugin-api)
# 跨平台动态库加载子项目（设计说明见 DLLLoder/README.md）
add_subdirectory(DLLLoder)

add_library(plugin_manager STATIC
    # main.cpp
    plugin_factory.h
    plugin_factory.cpp
)

add_library(plugin::manager ALIAS plugin_manager)

target_include_directories(plugin_manager
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_link_libraries(plugin_manager PRIVATE
    plugin_api
    dll_loader
)