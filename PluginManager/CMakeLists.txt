add_subdirectory(common)

add_library(plugin_manager STATIC
    plugin_factory.h
    plugin_factory.cpp
    utils/utils.h
    utils/utils.cpp
    metainfo.h
)

add_library(plugin::manager ALIAS plugin_manager)

target_include_directories(plugin_manager
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    PRIVATE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/utils>
)

target_link_libraries(plugin_manager 
    PUBLIC
        plugin_api
    PRIVATE
        plugin::aspect
        dll_loader
        # 仅内部实现使用（utils/MetainfoParser.*）。约定：
        # 任何对外头文件都不得引用 nlohmann/json，否则必须改为 PUBLIC。
        nlohmann_json
)