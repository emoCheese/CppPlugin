# DLLLoderWarnings.cmake —— 统一的告警与消毒开关
#
# 职责：把“严格但可移植”的编译告警与 sanitizer 配置收敛到一处，
# 供公共库、平台后端与测试复用，避免每个 target 各写一份。
# 说明：编译器差异在这里用 CMake 的 if() 区分（与源码无关）。

include_guard(GLOBAL)

# 为单个 target 打开严格告警。
function(dllloder_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus)
        if(DLLLODER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
            -Wold-style-cast
            -Wcast-align
            -Woverloaded-virtual
            -Wconversion
            -Wsign-conversion
            -Wdouble-promotion
            -Wformat=2
            -Wnull-dereference
            -Wimplicit-fallthrough
            -Wpointer-arith)

        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(${target} PRIVATE
                -Wuseless-cast
                -Wduplicated-cond
                -Wduplicated-branches
                -Wlogical-op)
        endif()

        if(DLLLODER_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# 当 DLLLODER_ENABLE_SANITIZERS=ON 时，为“当前目录及子目录”统一打开
# AddressSanitizer + UndefinedBehaviorSanitizer（编译与链接都要带）。
function(dllloder_apply_sanitizers)
    if(NOT DLLLODER_ENABLE_SANITIZERS)
        return()
    endif()

    if(MSVC)
        add_compile_options(/fsanitize=address)
        add_link_options(/fsanitize=address)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        add_link_options(-fsanitize=address,undefined)
    else()
        message(WARNING "DLLLoder: 编译器 ${CMAKE_CXX_COMPILER_ID} 不支持自动开启 sanitizer，已忽略 DLLLODER_ENABLE_SANITIZERS")
    endif()
endfunction()
