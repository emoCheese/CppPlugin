// test_dynamic_library.cpp —— DLLLoder 单元测试
//
// 不依赖第三方测试框架：极简 CHECK / CHECK_EQ + CTest 注册。
// 被测职责只有三项：加载、卸载、符号获取。
//
// 覆盖范围：
//   * 未打开句柄的语义、加载 / 卸载、卸载幂等、卸载后重新加载；
//   * 错误分类（空路径、文件不存在、非动态库文件）与 describe_error()；
//   * 函数符号与数据符号解析、缺失符号的诊断文本；
//   * 移动构造 / 移动赋值 / 同一文件两个句柄各自独立卸载；
//   * LoadFlags 组合在各平台的可编译与可用性；
//   * std::error_code 互操作。

#include "dll_loader/dynamic_library.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

int g_checks = 0;
int g_failures = 0;

/// 样例动态库路径：优先取命令行参数，其次取 CMake 注入的编译定义。
std::filesystem::path g_sample_plugin{DLLLOADER_TEST_SAMPLE_PLUGIN};

// ---- 极简断言 --------------------------------------------------------------

template <class T>
[[nodiscard]] std::string describe(const T& value)
{
    if constexpr (requires(std::ostringstream& stream, const T& item) { stream << item; }) {
        std::ostringstream stream;
        stream << value;
        return stream.str();
    } else {
        return "<无法打印的值>";
    }
}

[[nodiscard]] std::string describe(const std::error_code& code)
{
    std::ostringstream stream;
    stream << "error_code{value=" << code.value() << ", category=" << code.category().name()
           << ", message=\"" << code.message() << "\"}";
    return stream.str();
}

[[nodiscard]] std::string describe(dll_loader::ErrorCode code)
{
    return "ErrorCode{" + std::string{dll_loader::to_string(code)} + "}";
}

void check(bool passed,
           std::string_view expression,
           std::string_view detail,
           const char* file,
           int line)
{
    ++g_checks;
    if (passed) {
        return;
    }

    ++g_failures;
    std::fprintf(stderr, "  [失败] %s:%d\n         断言: %.*s\n", file, line,
                 static_cast<int>(expression.size()), expression.data());
    if (!detail.empty()) {
        std::fprintf(stderr, "         实际: %.*s\n",
                     static_cast<int>(detail.size()), detail.data());
    }
}

#define CHECK(expression) \
    check((expression), #expression, {}, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        const auto& dllloader_actual = (actual);                                      \
        const auto& dllloader_expected = (expected);                                  \
        check(dllloader_actual == dllloader_expected,                                 \
              #actual " == " #expected,                                               \
              describe(dllloader_actual) + "   vs   " + describe(dllloader_expected), \
              __FILE__, __LINE__);                                                    \
    } while (false)

// ---- 辅助 ------------------------------------------------------------------

/// 用严格默认标志加载样例库。
[[nodiscard]] std::optional<dll_loader::DynamicLibrary> open_sample(std::error_code& ec)
{
    return dll_loader::DynamicLibrary::open(g_sample_plugin, dll_loader::LoadFlags::none, ec);
}

using AddFunction = int (*)(int, int);
using GreetingFunction = const char* (*)();

// ---- 测试用例 --------------------------------------------------------------

void test_default_constructed_handle_is_closed()
{
    dll_loader::DynamicLibrary library;

    CHECK(!library.is_open());

    // 未打开的句柄上解析符号：必须安全失败并给出原因
    CHECK(library.find_symbol("sample_add") == nullptr);
    CHECK(!dll_loader::last_error_detail().empty());

    // 未打开的句柄上卸载：安全的空操作，返回 false 表示“没有可卸载的库”
    CHECK(!library.unload());
    CHECK(!library.is_open());
}

void test_open_empty_path()
{
    std::error_code ec;
    const auto library = dll_loader::DynamicLibrary::open(std::filesystem::path{},
                                                          dll_loader::LoadFlags::none,
                                                          ec);

    CHECK(!library.has_value());
    CHECK_EQ(ec, dll_loader::ErrorCode::invalid_argument);
    CHECK(!dll_loader::describe_error(ec).empty());
}

void test_open_missing_file()
{
    const std::filesystem::path missing =
        g_sample_plugin.parent_path() / "dll_loader_no_such_library.so";

    std::error_code ec;
    const auto library = dll_loader::DynamicLibrary::open(missing, dll_loader::LoadFlags::none, ec);

    CHECK(!library.has_value());
    CHECK_EQ(ec, dll_loader::ErrorCode::file_not_found);
    CHECK(dll_loader::describe_error(ec).find("no_such_library") != std::string::npos);
}

void test_open_non_library_file()
{
    // 造一个“存在但不是动态库”的文件：先是普通文本，再补足长度，
    // 确保各平台都会判定为“格式不对”而不是“文件不存在”。
    const std::filesystem::path bogus =
        std::filesystem::temp_directory_path() / "dll_loader_not_a_library.bin";
    {
        std::ofstream out{bogus, std::ios::binary | std::ios::trunc};
        out << "这不是动态库，只是一段普通文本。\n";
        out << std::string(512, 'x');
    }

    std::error_code ec;
    const auto library = dll_loader::DynamicLibrary::open(bogus, dll_loader::LoadFlags::none, ec);

    CHECK(!library.has_value());
    CHECK_EQ(ec, dll_loader::ErrorCode::not_a_library);
    CHECK(!dll_loader::last_error_detail().empty());

    std::error_code ignored;
    std::filesystem::remove(bogus, ignored);
}

void test_open_and_resolve_function()
{
    std::error_code ec;
    const auto library = open_sample(ec);

    CHECK(library.has_value());
    if (!library) {
        std::fprintf(stderr, "         加载样例库失败: %s\n",
                     dll_loader::describe_error(ec).c_str());
        return;
    }

    CHECK(library->is_open());

    const AddFunction add = library->find_function<AddFunction>("sample_add");
    CHECK(add != nullptr);
    if (add != nullptr) {
        CHECK_EQ(add(2, 3), 5);
    }

    const GreetingFunction greeting = library->find_function<GreetingFunction>("sample_greeting");
    CHECK(greeting != nullptr);
    if (greeting != nullptr) {
        CHECK_EQ(std::string_view{greeting()}, std::string_view{"hello from sample plugin"});
    }

    // 成功的解析不应留下失败诊断
    CHECK(dll_loader::last_error_detail().empty());
}

void test_resolve_data_symbol()
{
    std::error_code ec;
    const auto library = open_sample(ec);
    CHECK(library.has_value());
    if (!library) {
        return;
    }

    void* const raw = library->find_symbol("sample_magic_number");
    CHECK(raw != nullptr);
    if (raw != nullptr) {
        CHECK_EQ(*static_cast<int*>(raw), 42);
    }
}

void test_resolve_missing_symbol()
{
    std::error_code ec;
    const auto library = open_sample(ec);
    CHECK(library.has_value());
    if (!library) {
        return;
    }

    CHECK(library->find_symbol("sample_does_not_exist") == nullptr);

    const std::string_view detail = dll_loader::last_error_detail();
    CHECK(!detail.empty());
    CHECK(detail.find("sample_does_not_exist") != std::string_view::npos);
}

void test_unload_is_idempotent()
{
    std::error_code ec;
    auto library = open_sample(ec);
    CHECK(library.has_value());
    if (!library) {
        return;
    }

    CHECK(library->is_open());
    CHECK(library->unload());   // 第一次：确实卸载了

    CHECK(!library->is_open());
    CHECK(library->find_symbol("sample_add") == nullptr);   // 卸载后符号不再可用

    CHECK(!library->unload());   // 二次卸载：安全的空操作
    CHECK(!library->is_open());
}

void test_reopen_after_unload()
{
    std::error_code ec;
    auto library = open_sample(ec);
    CHECK(library.has_value());
    if (!library) {
        return;
    }

    CHECK(library->unload());

    const auto reopened = open_sample(ec);
    CHECK(reopened.has_value());
    if (reopened) {
        CHECK(reopened->is_open());
        CHECK(reopened->find_symbol("sample_add") != nullptr);
    }
}

void test_move_constructor_transfers_ownership()
{
    std::error_code ec;
    auto source = open_sample(ec);
    CHECK(source.has_value());
    if (!source) {
        return;
    }

    const dll_loader::DynamicLibrary moved{std::move(*source)};

    CHECK(moved.is_open());
    CHECK(moved.find_symbol("sample_add") != nullptr);

    // 被移动的源对象回到“未打开”状态，且仍然安全可用
    CHECK(!source->is_open());
    CHECK(source->find_symbol("sample_add") == nullptr);
}

void test_move_assignment_releases_target()
{
    std::error_code ec;
    auto target = open_sample(ec);
    auto source = open_sample(ec);
    CHECK(target.has_value());
    CHECK(source.has_value());
    if (!target || !source) {
        return;
    }

    *target = std::move(*source);   // 赋值前必须先释放 target 自己持有的库

    CHECK(target->is_open());
    CHECK(target->find_function<AddFunction>("sample_add") != nullptr);
    CHECK(!source->is_open());
}

void test_two_handles_unload_independently()
{
    std::error_code ec;
    auto first = open_sample(ec);
    auto second = open_sample(ec);
    CHECK(first.has_value());
    CHECK(second.has_value());
    if (!first || !second) {
        return;
    }

    // 同一文件加载两次：卸载其一不应影响另一个（平台引用计数语义）
    CHECK(first->unload());
    CHECK(second->is_open());
    CHECK(second->find_symbol("sample_add") != nullptr);

    CHECK(second->unload());
    CHECK(!second->is_open());
}

void test_load_flags_are_accepted()
{
    std::error_code ec;

    dll_loader::LoadFlags flags = dll_loader::LoadFlags::lazy_binding
                                | dll_loader::LoadFlags::global_visibility
                                | dll_loader::LoadFlags::keep_loaded;

    // Linux 的 RTLD_DEEPBIND 与 sanitizer 运行时不兼容（sanitizer 会终止进程），
    // 因此 sanitizer 构建下不把该标志纳入测试；开关由 CMake 注入。
    if constexpr (DLLLOADER_TEST_ALLOW_DEEP_BINDING != 0) {
        flags |= dll_loader::LoadFlags::deep_binding;
    } else {
        std::printf("        （sanitizer 构建：跳过 deep_binding 标志）\n");
    }

    // 位运算与查询
    CHECK(dll_loader::has_flag(flags, dll_loader::LoadFlags::keep_loaded));
    CHECK(!dll_loader::has_flag(dll_loader::LoadFlags::none, dll_loader::LoadFlags::keep_loaded));

    const auto library = dll_loader::DynamicLibrary::open(g_sample_plugin, flags, ec);
    CHECK(library.has_value());
    if (!library) {
        std::fprintf(stderr, "         带 flags 加载失败: %s\n",
                     dll_loader::describe_error(ec).c_str());
        return;
    }
    CHECK(!ec);
    CHECK(library->find_symbol("sample_add") != nullptr);
}

void test_error_code_interop()
{
    const std::error_code ec = dll_loader::ErrorCode::file_not_found;

    CHECK(static_cast<bool>(ec));
    CHECK_EQ(std::string_view{ec.category().name()}, std::string_view{"dll_loader"});
    // message() 只由错误码决定：稳定、可复现，不掺杂线程局部的诊断状态。
    // 注意：message() 返回 std::string，必须先落到具名变量再取 view。
    const std::string message = ec.message();
    CHECK_EQ(std::string_view{message},
             dll_loader::to_string(dll_loader::ErrorCode::file_not_found));
    CHECK_EQ(ec, dll_loader::make_error_code(dll_loader::ErrorCode::file_not_found));
    CHECK(ec != dll_loader::ErrorCode::symbol_not_found);
    CHECK_EQ(std::string_view{dll_loader::to_string(dll_loader::ErrorCode::not_a_library)},
             std::string_view{"not a loadable dynamic library"});
}

struct TestCase {
    std::string_view name;
    void (*run)();
};

}  // namespace

int main(int argc, char** argv)
{
    if (argc > 1) {
        g_sample_plugin = std::filesystem::path{argv[1]};
    }

    const std::array<TestCase, 14> cases{{
        {"未打开句柄的语义", &test_default_constructed_handle_is_closed},
        {"空路径 -> invalid_argument", &test_open_empty_path},
        {"文件不存在 -> file_not_found", &test_open_missing_file},
        {"非动态库文件 -> not_a_library", &test_open_non_library_file},
        {"加载并解析函数符号", &test_open_and_resolve_function},
        {"解析数据符号", &test_resolve_data_symbol},
        {"缺失符号的诊断", &test_resolve_missing_symbol},
        {"卸载幂等", &test_unload_is_idempotent},
        {"卸载后可重新加载", &test_reopen_after_unload},
        {"移动构造转移所有权", &test_move_constructor_transfers_ownership},
        {"移动赋值释放原有库", &test_move_assignment_releases_target},
        {"两个句柄各自独立卸载", &test_two_handles_unload_independently},
        {"LoadFlags 组合可用", &test_load_flags_are_accepted},
        {"error_code 互操作", &test_error_code_interop},
    }};

    std::printf("样例动态库: %s\n\n", g_sample_plugin.string().c_str());

    for (const TestCase& test : cases) {
        const int failures_before = g_failures;
        test.run();
        std::printf("[%s] %.*s\n",
                    g_failures == failures_before ? "通过" : "失败",
                    static_cast<int>(test.name.size()),
                    test.name.data());
    }

    std::printf("\n断言 %d 项，失败 %d 项\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
