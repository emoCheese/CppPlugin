set(PLUGIN_NAME plugin_a)


add_library(${PLUGIN_NAME} SHARED
    plugin_a.h plugin_a.cpp
)

# 去掉 lib 前缀，生成 plugin_a.so 而不是 libplugin_a.so
set_target_properties(${PLUGIN_NAME} PROPERTIES
    PREFIX ""
)

target_link_libraries(${PLUGIN_NAME}
    PRIVATE
        plugin::api
)

# 插件是内部使用，只需要构建时能找到自己的头文件
target_include_directories(${PLUGIN_NAME}
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        # $<INSTALL_INTERFACE:include/plugin_a>
)

install(TARGETS ${PLUGIN_NAME}
    # EXPORT ${PLUGIN_NAME}Targets  # 内部使用不需要导出
    LIBRARY DESTINATION plugin/${PLUGIN_NAME}
    RUNTIME DESTINATION plugin/${PLUGIN_NAME}
    ARCHIVE DESTINATION plugin/${PLUGIN_NAME}
)

# 内部使用不需要安装头文件
# install(FILES
#     plugin_a.h
#     DESTINATION include/plugin_a
# )

# ---- 安装 metainfo.json 和 库文件 到同一个目录 ----
install(FILES 
    metainfo.json
    DESTINATION plugin/${PLUGIN_NAME}
)