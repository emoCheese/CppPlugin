# DLLLoder —— 跨平台动态库加载 / 卸载 / 符号获取（子项目，唯一构建脚本）
#
# 本文件承担子项目的全部构建职责：
#   1) 选项（测试 / 告警即错误 / sanitizer）；
#   2) ★ 平台选择表：按目标平台挑选要编译的后端 .cpp，并声明该平台自己的编译要求；
#   3) 组装公共静态库 dll_loader；
#   4) 可选的样例动态库与单元测试。
#
# 平台差异只出现在下面的“平台选择表”里，后端源文件内部没有任何 #if 平台判断。
#
# 常用配置：
#   -DDLLLODER_BUILD_TESTS=OFF          不构建测试（作为子项目嵌入其它工程时常用）
#   -DDLLLODER_WARNINGS_AS_ERRORS=ON   把编译告警视为错误
#   -DDLLLODER_ENABLE_SANITIZERS=ON     打开 ASan + UBSan
#   -DDLLLODER_BACKEND=<windows|linux>  手工指定后端（默认 auto，按目标平台探测）

cmake_minimum_required(VERSION 3.16)

project(DLLLoder
    VERSION 1.0.0
    DESCRIPTION "跨平台动态库加载封装"
    LANGUAGES CXX)

# PROJECT_IS_TOP_LEVEL 需要 CMake 3.21；这里手写判断以兼容更老的 CMake
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(DLLLODER_IS_TOP_LEVEL ON)
else()
    set(DLLLODER_IS_TOP_LEVEL OFF)
endif()

# ================================ 选项 ======================================

# 作为子项目被引入时，默认跟随标准的 BUILD_TESTING 开关
if(DEFINED BUILD_TESTING)
    set(DLLLODER_TESTS_DEFAULT "${BUILD_TESTING}")
else()
    set(DLLLODER_TESTS_DEFAULT ON)
endif()

option(DLLLODER_BUILD_TESTS "构建 DLLLoder 单元测试" ${DLLLODER_TESTS_DEFAULT})
option(DLLLODER_WARNINGS_AS_ERRORS "把编译告警视为错误" OFF)
option(DLLLODER_ENABLE_SANITIZERS "启用 AddressSanitizer / UndefinedBehaviorSanitizer" OFF)

set(DLLLODER_BACKEND "auto" CACHE STRING "平台后端：auto | windows | linux")
set_property(CACHE DLLLODER_BACKEND PROPERTY STRINGS auto windows linux)

include(cmake/DLLLoderWarnings.cmake)
dllloder_apply_sanitizers()

set(DLLLODER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")

# ==================== 平台选择表（唯一区分平台的地方）=======================
#
# 目标平台 → 后端源文件。auto 时按 CMAKE_SYSTEM_NAME 探测；
# 也可以用 -DDLLLODER_BACKEND=<平台> 手工指定（交叉编译、调试新后端时用）。
#
# 新增一个平台：
#   1) 新建 src/<平台>/dynamic_library_<平台>.cpp，实现公共头的后端契约（见 README）；
#   2) 在下面的“探测表”与“后端表”里各加一段；
#   3) 公共头文件、既有后端、测试都不需要改动（开闭原则）。

# ---- 探测表：目标平台 → 后端名 ----
if(DLLLODER_BACKEND STREQUAL "auto")
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(DLLLODER_BACKEND_EFFECTIVE windows)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(DLLLODER_BACKEND_EFFECTIVE linux)
    else()
        set(DLLLODER_BACKEND_EFFECTIVE "")
    endif()
else()
    set(DLLLODER_BACKEND_EFFECTIVE "${DLLLODER_BACKEND}")
endif()

if(DLLLODER_BACKEND_EFFECTIVE STREQUAL "")
    message(FATAL_ERROR
        "DLLLoder: 目标平台 '${CMAKE_SYSTEM_NAME}' 暂无对应后端。\n"
        "当前可用后端：windows、linux。\n"
        "新增平台的做法：建立 src/<平台>/dynamic_library_<平台>.cpp，"
        "并在 DLLLoder/CMakeLists.txt 的平台选择表里加一段；"
        "或用 -DDLLLODER_BACKEND=<windows|linux> 指定已有后端。")
endif()

# ---- 后端表：后端名 → 源文件 + 该平台自己的编译要求 ----
if(DLLLODER_BACKEND_EFFECTIVE STREQUAL "windows")
    set(DLLLODER_BACKEND_SOURCE "src/windows/dynamic_library_windows.cpp")
    # kernel32（LoadLibraryExW / GetProcAddress / ...）由 MSVC 与 MinGW 隐式链接
    set(DLLLODER_BACKEND_DEFINITIONS "")
    set(DLLLODER_BACKEND_LIBRARIES "")
elseif(DLLLODER_BACKEND_EFFECTIVE STREQUAL "linux")
    set(DLLLODER_BACKEND_SOURCE "src/linux/dynamic_library_linux.cpp")
    # glibc 的 RTLD_DEEPBIND 位于 __USE_GNU 之后：显式声明本后端的这项依赖
    set(DLLLODER_BACKEND_DEFINITIONS _GNU_SOURCE)
    # libdl：glibc 2.34 起 dlopen 已并入 libc，CMAKE_DL_LIBS 会给出该平台的正确取值
    set(DLLLODER_BACKEND_LIBRARIES "${CMAKE_DL_LIBS}")
else()
    message(FATAL_ERROR
        "DLLLoder: 未知后端 '${DLLLODER_BACKEND_EFFECTIVE}'；可用后端为：windows、linux。")
endif()

message(STATUS "DLLLoder: 平台后端 = ${DLLLODER_BACKEND_EFFECTIVE}"
               " (CMAKE_SYSTEM_NAME=${CMAKE_SYSTEM_NAME})，编译 ${DLLLODER_BACKEND_SOURCE}")

# ================================ 目标 ======================================

# 公共静态库：平台实现 + 公共头，对外只有一个产物（libdll_loader.a）。
# 源列表里的 ${DLLLODER_BACKEND_SOURCE} 就是平台表挑中的那一份实现 ——
# 没有被选中的平台源文件根本不会进入构建图。
add_library(dll_loader STATIC)
add_library(APlugin::dll_loader ALIAS dll_loader)

target_sources(dll_loader PRIVATE
    "${DLLLODER_BACKEND_SOURCE}"
    include/dll_loader/dynamic_library.hpp
    include/dll_loader/load_error.hpp)

target_include_directories(dll_loader PUBLIC
    "$<BUILD_INTERFACE:${DLLLODER_INCLUDE_DIR}>")

target_compile_features(dll_loader PUBLIC cxx_std_20)

# 目标文件要能进共享库，也能进可执行文件
set_target_properties(dll_loader PROPERTIES POSITION_INDEPENDENT_CODE ON)

# 平台自己的编译要求：只作用于本库的编译，不污染使用者的接口
if(DLLLODER_BACKEND_DEFINITIONS)
    target_compile_definitions(dll_loader PRIVATE ${DLLLODER_BACKEND_DEFINITIONS})
endif()

if(DLLLODER_BACKEND_LIBRARIES)
    # 静态库的 PRIVATE 链接依赖会以 $<LINK_ONLY:...> 继续传递给最终链接，
    # 因此使用者链接 dll_loader 时会自动带上 -ldl，不需要自己写。
    target_link_libraries(dll_loader PRIVATE ${DLLLODER_BACKEND_LIBRARIES})
endif()

dllloder_set_warnings(dll_loader)

# ================================ 测试 ======================================

if(DLLLODER_BUILD_TESTS)
    if(DLLLODER_IS_TOP_LEVEL)
        enable_testing()
    endif()

    include(GenerateExportHeader)

    # 样例动态库：导出函数与数据符号；导出宏由 CMake 生成，
    # 样例源码里因此不需要任何平台判断（Windows 的 __declspec(dllexport) 也在生成的头里）。
    add_library(sample_plugin SHARED
        tests/fixtures/sample_plugin.cpp)

    target_compile_features(sample_plugin PRIVATE cxx_std_20)

    generate_export_header(sample_plugin
        BASE_NAME sample_plugin
        EXPORT_MACRO_NAME SAMPLE_PLUGIN_EXPORT
        EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/sample_plugin_export.h")

    target_include_directories(sample_plugin PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}")

    dllloder_set_warnings(sample_plugin)

    # 单元测试：不依赖第三方框架
    add_executable(dll_loader_tests
        tests/test_dynamic_library.cpp)

    target_link_libraries(dll_loader_tests PRIVATE dll_loader)
    dllloder_set_warnings(dll_loader_tests)

    # 样例库路径：既通过编译定义注入（便于手工运行），也通过命令行参数传入（CTest 用）
    target_compile_definitions(dll_loader_tests PRIVATE
        DLLLOADER_TEST_SAMPLE_PLUGIN="$<TARGET_FILE:sample_plugin>")

    # Linux 的 RTLD_DEEPBIND 与 sanitizer 运行时不兼容（会直接终止进程），
    # 因此带 sanitizer 的构建里让测试跳过 deep_binding 标志。
    if(DLLLODER_ENABLE_SANITIZERS)
        target_compile_definitions(dll_loader_tests PRIVATE DLLLOADER_TEST_ALLOW_DEEP_BINDING=0)
    else()
        target_compile_definitions(dll_loader_tests PRIVATE DLLLOADER_TEST_ALLOW_DEEP_BINDING=1)
    endif()

    add_dependencies(dll_loader_tests sample_plugin)

    add_test(NAME dll_loader.unit
        COMMAND dll_loader_tests "$<TARGET_FILE:sample_plugin>")
endif()
