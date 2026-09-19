// load_error.hpp —— 加载失败的分类与诊断（平台无关的公共 API）
//
// 职责：
//   1. 定义与平台无关的失败原因枚举 ErrorCode；
//   2. 把它接入 <system_error> 体系（可直接当 std::error_code 用）；
//   3. 暴露最近一次失败的“平台原始诊断文本”，用于日志与排障。
//
// 本头文件不包含任何平台相关代码，所有平台后端都实现同一套语义。

#pragma once

#include <format>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace dll_loader {

/// 动态库加载 / 符号解析的失败原因。
///
/// 平台后端负责把原生错误（POSIX 的 dlerror() 文本、Windows 的 GetLastError()）
/// 归一化到这里；无法归类时取 ErrorCode::unknown，
/// 而原始诊断文本始终可以通过 last_error_detail() 取得。
enum class ErrorCode : int {
    success = 0,         ///< 保留值，失败路径不会返回它
    invalid_argument,    ///< 入参非法（空路径等）
    file_not_found,      ///< 目标文件不存在，或其依赖项缺失
    not_a_library,       ///< 文件存在但不是本平台可加载的动态库（格式错误 / 架构不匹配 / 初始化失败）
    permission_denied,   ///< 权限不足
    symbol_not_found,    ///< 库中不存在所请求的符号
    out_of_memory,       ///< 内存分配失败
    unknown,             ///< 其他平台错误，详见 last_error_detail()
};

/// 稳定的英文短描述：不随平台与语言环境变化，适合直接写日志。
[[nodiscard]] inline std::string_view to_string(ErrorCode code) noexcept
{
    switch (code) {
        case ErrorCode::success:           return "success";
        case ErrorCode::invalid_argument:  return "invalid argument";
        case ErrorCode::file_not_found:    return "library file not found";
        case ErrorCode::not_a_library:     return "not a loadable dynamic library";
        case ErrorCode::permission_denied: return "permission denied";
        case ErrorCode::symbol_not_found:  return "symbol not found";
        case ErrorCode::out_of_memory:     return "out of memory";
        case ErrorCode::unknown:           return "unknown error";
    }
    return "unknown error";
}

/// 最近一次失败的平台原始诊断文本（线程局部）。
///
/// 约定：每个公共接口在进入时清空、失败时写入 —— 因此调用返回后
/// “detail 为空” 等价于 “本次调用成功”。
/// 返回的 string_view 指向线程局部缓冲，在下一次 dll_loader 调用之前有效。
///
/// 由各平台后端实现（一个平台一份独立的 .cpp）。
[[nodiscard]] std::string_view last_error_detail() noexcept;

namespace detail {

/// std::error_category 实现。
///
/// message() 只由 ErrorCode 决定（不含线程局部状态），因此：
///   * 结果稳定、可复现，跨线程使用同一个 error_code 也不会变；
///   * 平台原始诊断请用 describe_error() 或 last_error_detail()。
class ErrorCategory final : public std::error_category {
public:
    [[nodiscard]] const char* name() const noexcept override { return "dll_loader"; }

    [[nodiscard]] std::string message(int condition) const override
    {
        return std::string{to_string(static_cast<ErrorCode>(condition))};
    }
};

} // namespace detail

/// dll_loader 的错误分类目录（进程内唯一实例）。
[[nodiscard]] inline const std::error_category& error_category() noexcept
{
    static const detail::ErrorCategory instance{};   // 局部静态：C++11 起初始化线程安全
    return instance;
}

/// 让 std::error_code 能通过 ADL 从 ErrorCode 构造。
[[nodiscard]] inline std::error_code make_error_code(ErrorCode code) noexcept
{
    return std::error_code{static_cast<int>(code), error_category()};
}

/// “分类描述 + 当前线程的平台原始诊断”，适合失败后立刻写日志。
///
/// 注意：平台诊断来自线程局部缓冲，语义是“刚刚发生了什么”，
/// 因此请在失败返回后立即调用，不要把它当作 error_code 的固有属性保存。
[[nodiscard]] inline std::string describe_error(const std::error_code& ec)
{
    std::string text = ec.message();
    const std::string_view detail = last_error_detail();
    if (ec.category() == error_category() && !detail.empty()) {
        text = std::format("{}: {}", text, detail);
    }
    return text;
}

} // namespace dll_loader

namespace std {

template <>
struct is_error_code_enum<dll_loader::ErrorCode> : true_type {};

} // namespace std
