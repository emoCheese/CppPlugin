cmake_minimum_required(VERSION 3.20.0)
project(APlugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 测试基础设施：提供 BUILD_TESTING 开关并 enable_testing()
include(CTest)

add_subdirectory(PluginManager)

# add_subdirectory(plugin-api)
add_subdirectory(plugin_test/plugin_a)




