// sample_plugin.cpp —— 测试用样例动态库
//
// 导出内容：
//   * C ABI 函数（加、问候、计数）；
//   * 一个 int 数据符号，用来验证 loader 对“变量符号”的解析能力。
//
// 导出宏 SAMPLE_PLUGIN_EXPORT 由 CMake 的 generate_export_header 生成，
// 因此本文件不需要任何平台判断。

#include "sample_plugin_export.h"

namespace {

int g_call_count = 0;

const char* const kGreeting = "hello from sample plugin";

}  // namespace

extern "C" {

SAMPLE_PLUGIN_EXPORT int sample_add(int lhs, int rhs)
{
    return lhs + rhs;
}

SAMPLE_PLUGIN_EXPORT const char* sample_greeting()
{
    return kGreeting;
}

SAMPLE_PLUGIN_EXPORT void sample_bump()
{
    ++g_call_count;
}

SAMPLE_PLUGIN_EXPORT int sample_call_count()
{
    return g_call_count;
}

/// 数据符号：通过 find_symbol() 解析后按 int* 读取。
SAMPLE_PLUGIN_EXPORT int sample_magic_number = 42;

}  // extern "C"
