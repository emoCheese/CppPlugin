// dynamic_library_windows.cpp —— Windows（Win32 API）平台后端
//
// 原生接口：LoadLibraryExW / GetProcAddress / FreeLibrary / GetModuleHandleExW。
//
// 严格加载策略：
//   * 先转成绝对路径再加载，并显式使用 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
//     LOAD_LIBRARY_SEARCH_DEFAULT_DIRS，避免“当前目录 / PATH”带来的 DLL 劫持；
//   * 老系统不支持 LOAD_LIBRARY_SEARCH_* 时回退 LOAD_WITH_ALTERED_SEARCH_PATH；
//   * keep_loaded 通过 GetModuleHandleExW(PIN) 真正固定模块，失败则整体加载失败
//     （明确请求的能力不会被静默忽略）。
//
// 本文件只在 Windows 目标上被编译（由 CMake 选择，见 src/CMakeLists.txt），
// 因此内部不含任何“判断平台”的预处理指令。
//
// 注：文件开头的两个宏是 windows.h 的常规“瘦身”配置，与平台选择无关。

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include "dll_loader/dynamic_library.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
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

/// LocalFree 的 RAII 守卫，保证 FormatMessageW 分配的缓冲区一定被释放。
class LocalFreeGuard final {
public:
    explicit LocalFreeGuard(HLOCAL block) noexcept : block_{block} {}

    ~LocalFreeGuard()
    {
        if (block_ != nullptr) {
            ::LocalFree(block_);
        }
    }

    LocalFreeGuard(const LocalFreeGuard&) = delete;
    LocalFreeGuard& operator=(const LocalFreeGuard&) = delete;
    LocalFreeGuard(LocalFreeGuard&&) = delete;
    LocalFreeGuard& operator=(LocalFreeGuard&&) = delete;

private:
    HLOCAL block_;
};

/// 宽字符 → UTF-8；失败返回空串（诊断信息尽力而为）。
[[nodiscard]] std::string wide_to_utf8(std::wstring_view text)
{
    if (text.empty()) {
        return {};
    }

    const int source_length = static_cast<int>(text.size());
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), source_length,
                                               nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }

    std::string utf8(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), source_length,
                                              utf8.data(), required, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    return utf8;
}

/// 把 Win32 错误码转换成可读文本（系统语言）。
[[nodiscard]] std::string format_win32_message(DWORD error) noexcept
{
    try {
        wchar_t* buffer = nullptr;
        const DWORD length = ::FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
            nullptr,
            error,
            MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
            reinterpret_cast<LPWSTR>(&buffer),
            0,
            nullptr);

        if (buffer == nullptr) {
            return {};
        }

        const LocalFreeGuard guard{buffer};   // 分配成功后立刻上锁，任何分支都会释放

        if (length == 0) {
            return {};
        }

        std::wstring_view message{buffer, length};
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n'
                                    || message.back() == L' ' || message.back() == L'.')) {
            message.remove_suffix(1);
        }
        return wide_to_utf8(message);
    } catch (...) {
        return {};
    }
}

/// "Win32 error 126: 找不到指定的模块。"
[[nodiscard]] std::string describe_win32_error(DWORD error) noexcept
{
    std::string text;
    try {
        text = "Win32 error ";
        text += std::to_string(error);

        const std::string message = format_win32_message(error);
        if (!message.empty()) {
            text += ": ";
            text += message;
        }
    } catch (...) {
        // 诊断信息尽力而为：内存不足时退化为已有前缀
    }
    return text;
}

/// 把 Win32 错误码归类到 ErrorCode；原始文本始终保留在 last_error_detail() 中。
[[nodiscard]] ErrorCode classify_win32_error(DWORD error) noexcept
{
    switch (error) {
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
        case ERROR_MOD_NOT_FOUND:            // 依赖项缺失也归入“找不到”
            return ErrorCode::file_not_found;
        case ERROR_BAD_EXE_FORMAT:
        case ERROR_EXE_MACHINE_TYPE_MISMATCH:
        case ERROR_DLL_INIT_FAILED:
            return ErrorCode::not_a_library;
        case ERROR_ACCESS_DENIED:
            return ErrorCode::permission_denied;
        case ERROR_NOT_ENOUGH_MEMORY:
        case ERROR_OUTOFMEMORY:
            return ErrorCode::out_of_memory;
        case ERROR_PROC_NOT_FOUND:
            return ErrorCode::symbol_not_found;
        default:
            return ErrorCode::unknown;
    }
}

/// 把 LoadFlags 翻译成 LoadLibraryEx 标志。
///
/// Windows 在加载导入表时即完成绑定，且符号默认对整个进程可见，
/// 因此 lazy_binding / global_visibility / deep_binding 在本平台没有对应能力，
/// 按 README 的平台矩阵忽略（keep_loaded 在 open() 中单独处理）。
[[nodiscard]] DWORD to_native_flags([[maybe_unused]] LoadFlags flags) noexcept
{
    return LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
}

/// HMODULE 的 RAII 守卫：构造 DynamicLibrary 过程中的任何失败都不会泄漏模块。
class HandleGuard final {
public:
    explicit HandleGuard(HMODULE handle) noexcept : handle_{handle} {}

    ~HandleGuard()
    {
        if (handle_ != nullptr) {
            ::FreeLibrary(handle_);
        }
    }

    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    HandleGuard(HandleGuard&&) = delete;
    HandleGuard& operator=(HandleGuard&&) = delete;

    /// 所有权已移交，不再由本守卫卸载。
    void release() noexcept { handle_ = nullptr; }

private:
    HMODULE handle_;
};

} // namespace

/// 平台相关状态：Windows 后端只需要一个 HMODULE。
struct DynamicLibrary::Impl {
    HMODULE handle = nullptr;
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

        ::SetLastError(ERROR_SUCCESS);
        HMODULE handle = ::LoadLibraryExW(absolute.c_str(), nullptr, to_native_flags(flags));

        if (handle == nullptr && ::GetLastError() == ERROR_INVALID_PARAMETER) {
            // 未安装 KB2533623 的老系统不认识 LOAD_LIBRARY_SEARCH_*，
            // 回退到 LOAD_WITH_ALTERED_SEARCH_PATH：仍然优先 DLL 自身所在目录。
            ::SetLastError(ERROR_SUCCESS);
            handle = ::LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        }

        if (handle == nullptr) {
            const DWORD error = ::GetLastError();
            set_error_detail(absolute.string() + " (" + describe_win32_error(error) + ")");
            ec = make_error_code(classify_win32_error(error));
            return std::nullopt;
        }

        HandleGuard guard{handle};   // 从这里到构造完成之间不会泄漏模块

        if (has_flag(flags, LoadFlags::keep_loaded)) {
            HMODULE pinned = nullptr;
            ::SetLastError(ERROR_SUCCESS);
            const DWORD result = ::GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(handle),
                &pinned);
            if (result == 0) {
                // 明确请求的能力无法满足 → 严格失败，而不是静默忽略
                const DWORD error = ::GetLastError();
                set_error_detail("GetModuleHandleExW(PIN) failed: " + describe_win32_error(error));
                ec = make_error_code(classify_win32_error(error));
                return std::nullopt;   // guard 负责 FreeLibrary
            }
        }

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
        // noexcept 边界必须兜住所有异常（例如路径宽窄字符转换失败）：
        // 宿主进程不应因为加载失败而 std::terminate()
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
        ::SetLastError(ERROR_SUCCESS);
        if (::FreeLibrary(impl_->handle) == 0) {
            // FreeLibrary 失败时模块可能已经释放：无论成败都清掉本地状态，
            // 只在 last_error_detail() 中保留原因，避免留下一个不可用的句柄。
            const DWORD error = ::GetLastError();
            set_error_detail("FreeLibrary failed: " + describe_win32_error(error));
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
        const std::string symbol{name};

        ::SetLastError(ERROR_SUCCESS);
        const FARPROC address = ::GetProcAddress(impl_->handle, symbol.c_str());
        if (address == nullptr) {
            const DWORD error = ::GetLastError();   // 通常是 ERROR_PROC_NOT_FOUND
            set_error_detail(symbol + " (" + describe_win32_error(error) + ")");
            return nullptr;
        }
        return reinterpret_cast<void*>(address);
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
