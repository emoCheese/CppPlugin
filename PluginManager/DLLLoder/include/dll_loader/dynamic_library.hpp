// dynamic_library.hpp —— 跨平台动态库加载与符号获取（唯一的稳定公共 API）
//
// 职责边界（刻意保持最小）：
//   * 只做两件事：加载 / 卸载动态库、解析符号地址；
//   * 插件的实例管理、生命周期编排、注册表等属于宿主层，不在本库内；
//   * 本头文件与平台无关：Linux / Windows 共用同一份声明，平台差异全部封在
//     私有成员 Impl 与各平台后端的 .cpp 里（由 CMake 选择编译哪一份）；
//   * 失败不抛异常：全部通过 std::error_code + last_error_detail() 报告；
//   * RAII：析构即卸载；不可拷贝；移动后源对象回到“未打开”状态。

#pragma once

#include "dll_loader/load_error.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace dll_loader {

/// 加载行为的位掩码。
///
/// 每个平台后端把可用位映射到自己的原生标志；平台没有对应能力时按下方说明忽略，
/// 完整矩阵见 README。默认值（LoadFlags::none）是“严格模式”：
/// 立即绑定 + 符号局部可见 + 按绝对路径加载。
enum class LoadFlags : std::uint32_t {
    none = 0u,

    /// 延迟绑定（POSIX: RTLD_LAZY）。
    /// 默认是立即绑定（RTLD_NOW）：加载期就解析全部重定位，坏库在 open() 就失败，
    /// 而不是等到第一次调用时才崩溃。
    lazy_binding = 1u << 0,

    /// 符号对后续加载的库可见（POSIX: RTLD_GLOBAL）。
    /// 默认 RTLD_LOCAL，避免插件污染进程全局符号空间。
    global_visibility = 1u << 1,

    /// 优先/限定使用库自身的符号（Linux: RTLD_DEEPBIND；Windows: 无对应能力，忽略）。
    ///
    /// 注意：Linux 的 RTLD_DEEPBIND 与 AddressSanitizer / UndefinedBehaviorSanitizer
    /// 运行时不兼容（sanitizer 会直接终止进程），带 sanitizer 的构建里不要使用本标志。
    deep_binding = 1u << 2,

    /// 卸载时不移除模块（Linux: RTLD_NODELETE；Windows: GetModuleHandleEx 固定模块）。
    keep_loaded = 1u << 3,
};

[[nodiscard]] constexpr LoadFlags operator|(LoadFlags lhs, LoadFlags rhs) noexcept
{
    return static_cast<LoadFlags>(static_cast<std::uint32_t>(lhs) | static_cast<std::uint32_t>(rhs));
}

[[nodiscard]] constexpr LoadFlags operator&(LoadFlags lhs, LoadFlags rhs) noexcept
{
    return static_cast<LoadFlags>(static_cast<std::uint32_t>(lhs) & static_cast<std::uint32_t>(rhs));
}

constexpr LoadFlags& operator|=(LoadFlags& lhs, LoadFlags rhs) noexcept
{
    lhs = lhs | rhs;
    return lhs;
}

/// flags 中是否包含 flag。
[[nodiscard]] constexpr bool has_flag(LoadFlags flags, LoadFlags flag) noexcept
{
    return (flags & flag) == flag;
}

/// 约束：只能指向函数的指针类型（挡掉对象指针与其它无关类型）。
template <class T>
concept FunctionPointer = std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

/// 已加载动态库的所有权句柄。
///
/// 语义要点：
///   * 一个对象 = 一次“加载”的所有权；同一文件可以打开多次，各自独立卸载
///     （与底层 dlopen / LoadLibrary 的引用计数语义一致）；
///   * 未打开的对象合法：is_open() == false，unload() 幂等，find_symbol() 返回 nullptr；
///   * 析构函数等价于 unload()；
///   * 线程安全：不同对象可跨线程并发使用；同一对象上 unload() 与其它调用需要调用方
///     自行同步；find_symbol() / find_function() 为 const、无内部缓存，可并发调用。
///
/// 本类是值类型，不通过继承扩展；扩展点在后端（新增平台 = 新增一个 src/<平台>/ 目录，
/// 公共头文件零改动），详见 README 的“如何新增一个平台后端”。
class DynamicLibrary final {
public:
    /// 创建一个“未打开”的句柄。
    ///
    /// 注意：构造函数与析构函数都刻意不在头文件里内联定义 —— Impl 是不完整类型，
    /// 内联定义会要求编译器在此处实例化 unique_ptr<Impl> 的析构（编译期报错）。
    DynamicLibrary() noexcept;

    ~DynamicLibrary();

    DynamicLibrary(DynamicLibrary&& other) noexcept;
    DynamicLibrary& operator=(DynamicLibrary&& other) noexcept;
    DynamicLibrary(const DynamicLibrary&) = delete;
    DynamicLibrary& operator=(const DynamicLibrary&) = delete;

    /// 加载动态库：本库唯一的加载入口。
    ///
    /// @param file  动态库路径；相对路径会先转成绝对路径再交给平台加载器，
    ///              避免平台按“当前目录 / 搜索路径”意外命中别的同名库。
    /// @param flags 加载行为位掩码；严格默认取 LoadFlags::none。
    /// @param ec    失败原因；调用开始时被清空，失败时写入。
    /// @return 成功返回句柄；失败返回 std::nullopt（原因见 ec 与 last_error_detail()）。
    [[nodiscard]] static std::optional<DynamicLibrary> open(const std::filesystem::path& file,
                                                           LoadFlags flags,
                                                           std::error_code& ec) noexcept;

    /// 卸载：open() 的逆操作，释放本对象持有的那一次加载。
    ///
    ///   * 幂等：未打开（或已卸载）时是安全的空操作；
    ///   * 不抛异常；失败原因写入 last_error_detail()；
    ///   * 释放的是“本对象这一次加载的所有权”，与底层 dlopen / LoadLibrary 的引用计数一致：
    ///     若还有其它持有者（另一个 DynamicLibrary、其它模块对它的依赖，或用 keep_loaded
    ///     固定的模块），模块会继续驻留内存 —— 这是平台的既定语义，不是失败；
    ///   * 卸载后，从该库解析出来的函数指针与数据指针全部失效，不得再使用。
    ///     宿主应先 shutdown() 并销毁插件对象，最后才卸载库（顺序见 README）。
    ///
    /// @return true  本次调用确实卸载了一个已加载的库；
    ///         false 本来就没有可卸载的库，或卸载失败（见 last_error_detail()）。
    bool unload() noexcept;

    /// 是否持有已加载的库。
    [[nodiscard]] bool is_open() const noexcept { return impl_ != nullptr; }

    /// 解析符号地址（函数或数据）。失败返回 nullptr，原因见 last_error_detail()。
    [[nodiscard]] void* find_symbol(std::string_view name) const noexcept;

    /// 解析函数符号并转成函数指针类型；失败返回 nullptr。
    ///
    /// 这里是全库唯一做 “对象指针 → 函数指针” 转换的地方，调用方无需自行
    /// reinterpret_cast。调用方必须保证 Fn 与该符号的真实签名一致
    /// （跨 ABI 的签名一致性无法由 C++ 类型系统校验）。
    template <FunctionPointer Fn>
    [[nodiscard]] Fn find_function(std::string_view name) const noexcept
    {
        return reinterpret_cast<Fn>(find_symbol(name));
    }

private:
    /// 平台相关状态；每个平台后端在自己的 .cpp 中定义（唯一的扩展点）。
    struct Impl;

    std::unique_ptr<Impl> impl_;   ///< 非空即表示已加载
};

} // namespace dll_loader
