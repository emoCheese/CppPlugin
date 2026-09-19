// dynamic_library_linux.cpp —— Linux/glibc 平台后端
//
// 原生接口：dlopen / dlsym / dlclose（libdl，glibc 2.34 起并入 libc）。
//
// 本文件只在 Linux 目标上被编译（由 CMake 选择，见 src/CMakeLists.txt），
// 因此内部不含任何“判断平台”的预处理指令：
// 新增平台 = 新增一个后端目录（cpp + CMakeLists），本文件零改动。

#include "dll_loader/dynamic_library.hpp"

#include <dlfcn.h>

#include <filesystem>
#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace dll_loader {
namespace {

/// 线程局部的失败诊断，保证 noexcept 的公共接口也能给出可读原因。
thread_local std::string tls_error_detail;

void clear_error_detail() noexcept
{
    tls_error_detail.clear();
}

void set_error_detail(std::string_view text) noexcept
{
    try {
        tls_error_detail.assign(text);
    } catch (...) {
        tls_error_detail.clear();   // 诊断信息尽力而为：绝不因为它再抛异常
    }
}

/// 把 LoadFlags 翻译成 dlopen 标志。Linux/glibc 支持全部四种行为。
[[nodiscard]] int to_native_flags(LoadFlags flags) noexcept
{
    int native = has_flag(flags, LoadFlags::lazy_binding) ? RTLD_LAZY : RTLD_NOW;
    native |= has_flag(flags, LoadFlags::global_visibility) ? RTLD_GLOBAL : RTLD_LOCAL;
    if (has_flag(flags, LoadFlags::deep_binding)) {
        native |= RTLD_DEEPBIND;
    }
    if (has_flag(flags, LoadFlags::keep_loaded)) {
        native |= RTLD_NODELETE;
    }
    return native;
}

/// 把 dlerror() 文本归类到 ErrorCode。
///
/// 仅用于“分类”：原始文本始终完整保留在 last_error_detail() 里，
/// 所以即使这里的映射表将来不匹配，诊断信息也不会丢失。
[[nodiscard]] ErrorCode classify_dl_error(std::string_view message) noexcept
{
    struct FragmentToCode {
        std::string_view fragment;
        ErrorCode code;
    };

    static constexpr FragmentToCode kTable[] = {
        {"cannot open shared object file", ErrorCode::file_not_found},
        {"No such file or directory", ErrorCode::file_not_found},
        {"invalid ELF header", ErrorCode::not_a_library},
        {"wrong ELF class", ErrorCode::not_a_library},
        {"file too short", ErrorCode::not_a_library},
        {"cannot dynamically load", ErrorCode::not_a_library},
        {"Permission denied", ErrorCode::permission_denied},
        {"cannot allocate memory", ErrorCode::out_of_memory},
        {"undefined symbol", ErrorCode::symbol_not_found},
    };

    for (const FragmentToCode& entry : kTable) {
        if (message.find(entry.fragment) != std::string_view::npos) {
            return entry.code;
        }
    }
    return ErrorCode::unknown;
}

/// dlopen 句柄的 RAII 守卫：构造 DynamicLibrary 过程中的任何失败都不会泄漏句柄。
class HandleGuard final {
public:
    explicit HandleGuard(void* handle) noexcept : handle_{handle} {}

    ~HandleGuard()
    {
        if (handle_ != nullptr) {
            ::dlclose(handle_);
        }
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&&) = delete;
    HandleGuard& operator=(HandleGuard&&) = delete;

    /// 所有权已移交，不再由本守卫卸载。
    void release() noexcept { handle_ = nullptr; }

private:
    void* handle_;
};

} // namespace

/// 平台相关状态：Linux 后端只需要一个 dlopen 句柄。
struct DynamicLibrary::Impl {
    void* handle = nullptr;
};

// ---- 生命周期 --------------------------------------------------------------

DynamicLibrary::DynamicLibrary() noexcept = default;

DynamicLibrary::~DynamicLibrary()
{
    unload();
}

DynamicLibrary::DynamicLibrary(DynamicLibrary&& other) noexcept
    : impl_{std::move(other.impl_)}
{
}

DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&& other) noexcept
{
    if (this != &other) {
        unload();   // 先释放自身持有的库
        impl_ = std::move(other.impl_);
    }
    return *this;   // 被移动的源对象此时已是“未打开”状态
}

// ---- 加载 / 卸载 -----------------------------------------------------------
// file 路径必须是  path/xxx.so
std::optional<DynamicLibrary> DynamicLibrary::open(const std::filesystem::path& file,
                                                   LoadFlags flags,
                                                   std::error_code& ec) noexcept
{
    ec.clear();
    clear_error_detail();

    if (file.empty()) {
        set_error_detail("library path is empty");
        ec = make_error_code(ErrorCode::invalid_argument);
        return std::nullopt;
    }

    try {
        std::error_code fs_error;
        const std::filesystem::path absolute = std::filesystem::absolute(file, fs_error);
        if (fs_error) {
            set_error_detail(fs_error.message());
            ec = make_error_code(ErrorCode::invalid_argument);
            return std::nullopt;
        }

        const bool exists = std::filesystem::exists(absolute, fs_error);
        if (fs_error) {
            set_error_detail(fs_error.message());
            ec = make_error_code(ErrorCode::unknown);
            return std::nullopt;
        }
        if (!exists) {
            // 诊断只放“具体路径”：通用描述由 error_code / describe_error() 提供，避免重复
            set_error_detail(absolute.string());
            ec = make_error_code(ErrorCode::file_not_found);
            return std::nullopt;
        }

        ::dlerror();   // POSIX 规定：调用前先清空历史错误状态
        void* const handle = ::dlopen(absolute.c_str(), to_native_flags(flags));
        if (handle == nullptr) {
            const char* const message = ::dlerror();
            const std::string_view detail =
                message != nullptr ? std::string_view{message} : std::string_view{"dlopen failed"};
            set_error_detail(detail);
            ec = make_error_code(classify_dl_error(detail));
            return std::nullopt;
        }

        HandleGuard guard{handle};   // 从这里到构造完成之间不会泄漏句柄

        DynamicLibrary library;
        library.impl_ = std::make_unique<DynamicLibrary::Impl>();
        library.impl_->handle = handle;
        guard.release();                   // 所有权移交给 library

        return std::optional<DynamicLibrary>{std::move(library)};
    } catch (const std::bad_alloc&) {
        set_error_detail("out of memory while opening library");
        ec = make_error_code(ErrorCode::out_of_memory);
        return std::nullopt;
    } catch (const std::exception& error) {
        // noexcept 边界必须兜住所有异常：宿主进程不应因为加载失败而 std::terminate()
        set_error_detail(error.what());
        ec = make_error_code(ErrorCode::unknown);
        return std::nullopt;
    } catch (...) {
        set_error_detail("unknown C++ exception while opening library");
        ec = make_error_code(ErrorCode::unknown);
        return std::nullopt;
    }
}

// ---- 卸载 ------------------------------------------------------------------

bool DynamicLibrary::unload() noexcept
{
    clear_error_detail();

    if (impl_ == nullptr) {
        return false;   // 本来就是“未打开”：安全的空操作
    }

    bool unloaded = true;
    if (impl_->handle != nullptr) {
        if (::dlclose(impl_->handle) != 0) {
            // dlclose 失败时模块可能已经卸载：无论成败都清掉本地状态，
            // 只在 last_error_detail() 中保留原因，避免留下一个不可用的句柄。
            const char* const message = ::dlerror();
            set_error_detail(message != nullptr ? std::string_view{message}
                                                : std::string_view{"dlclose failed"});
            unloaded = false;
        }
        impl_->handle = nullptr;
    }
    impl_.reset();
    return unloaded;
}

// ---- 符号解析 --------------------------------------------------------------

void* DynamicLibrary::find_symbol(std::string_view name) const noexcept
{
    clear_error_detail();

    if (impl_ == nullptr) {
        set_error_detail("library is not open");
        return nullptr;
    }

    try {
        const std::string symbol{name};   // dlsym 需要以 '\0' 结尾的名字

        ::dlerror();   // 必须先清空，dlsym 之后用 dlerror() 判断是否真的失败
        void* const address = ::dlsym(impl_->handle, symbol.c_str());
        const char* const message = ::dlerror();

        if (message != nullptr) {
            set_error_detail(message);
            return nullptr;
        }
        if (address == nullptr) {
            // dlsym 成功但地址为 0：POSIX 允许“符号恰好位于地址 0”的理论情形，
            // 无法与失败区分，这里按更严格的“未找到”处理。
            set_error_detail("symbol not found: " + symbol);
            return nullptr;
        }
        return address;
    } catch (const std::bad_alloc&) {
        set_error_detail("out of memory while resolving symbol");
        return nullptr;
    } catch (const std::exception& error) {
        set_error_detail(error.what());
        return nullptr;
    } catch (...) {
        set_error_detail("unknown C++ exception while resolving symbol");
        return nullptr;
    }
}

std::string_view last_error_detail() noexcept
{
    return tls_error_detail;
}

} // namespace dll_loader
