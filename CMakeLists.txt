cmake_minimum_required(VERSION 3.20.0)
project(APlugin VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 测试基础设施：提供 BUILD_TESTING 开关并 enable_testing()
include(CTest)

# 跨平台动态库加载子项目（设计说明见 DLLLoder/README.md）
add_subdirectory(DLLLoder)

add_executable(APlugin 
    # main.cpp
    plugin_api.h
    plugin_factory.h
    plugin_factory.cpp
)

