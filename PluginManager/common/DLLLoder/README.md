# DLLLoder —— 跨平台动态库加载 / 卸载 / 符号获取

一个 CMake 子项目：把 `dlopen` / `LoadLibraryEx` 这类平台原生加载接口，封装成**一套与平台无关、失败可诊断、生命周期严格**的 C++20 API。

职责边界只有三件事：**加载库、卸载库、取符号**。插件实例管理、注册表、生命周期编排属于宿主层（`PluginFactory`），不在本库内。

- 顶层头文件 `include/dll_loader/*.hpp` 是唯一稳定 API，Windows 与 Linux 完全共用；
- 一个平台一份**独立实现**（`src/<平台>/dynamic_library_<平台>.cpp`），由 **CMake 选择编译哪一份**，
  源文件里没有任何 `#if` 平台分支；
- 错误一律用 `std::error_code` 报告，不抛异常；平台原始诊断文本另附，便于排障；
- RAII：析构即卸载，不可拷贝、可移动，`unload()` 幂等；
- 默认是**严格模式**：立即绑定 + 符号局部可见 + 绝对路径加载。

---

## 1. 目录结构

```
DLLLoder/
├── CMakeLists.txt                  ★ 子项目唯一的构建脚本：
│                                     选项、平台选择表、公共库 dll_loader、测试
├── cmake/
│   └── DLLLoderWarnings.cmake      复用模块：统一告警集合 + sanitizer 开关
├── include/dll_loader/             ★ 唯一稳定 API（平台无关）
│   ├── dynamic_library.hpp         DynamicLibrary / LoadFlags / FunctionPointer
│   └── load_error.hpp              ErrorCode / describe_error / last_error_detail
├── src/
│   ├── linux/dynamic_library_linux.cpp      dlopen / dlsym / dlclose
│   └── windows/dynamic_library_windows.cpp  LoadLibraryExW / GetProcAddress / FreeLibrary
└── tests/
    ├── fixtures/sample_plugin.cpp  测试用样例库（导出函数与数据符号）
    └── test_dynamic_library.cpp    单元测试（不依赖第三方框架）
```

构建脚本只有一个：平台目录下**没有**各自的 `CMakeLists.txt`，编译哪一份 `.cpp` 完全由
`DLLLoder/CMakeLists.txt` 里的平台选择表决定。配置时会直接打印选中的源文件：

```
-- DLLLoder: 平台后端 = linux (CMAKE_SYSTEM_NAME=Linux)，编译 src/linux/dynamic_library_linux.cpp
```

---

## 2. 快速上手

```cpp
#include "dll_loader/dynamic_library.hpp"

#include <cstdio>
#include <iostream>

int main()
{
    std::error_code ec;
    auto library = dll_loader::DynamicLibrary::open("libsample_plugin.so",
                                                    dll_loader::LoadFlags::none,   // 严格默认
                                                    ec);
    if (!library) {
        std::cerr << "加载失败: " << dll_loader::describe_error(ec) << '\n';
        return 1;
    }

    using AddFunction = int (*)(int, int);
    const AddFunction add = library->find_function<AddFunction>("sample_add");
    if (add == nullptr) {
        std::cerr << "符号缺失: " << dll_loader::last_error_detail() << '\n';
        return 1;
    }

    std::printf("2 + 3 = %d\n", add(2, 3));

    // 显式卸载；不调用也可以 —— 析构时会自动卸载
    if (!library->unload()) {
        std::cerr << "卸载失败: " << dll_loader::last_error_detail() << '\n';
    }
    return 0;
}
```

### API 一览

整个公共 API 只有下面这些 —— 加载、卸载、符号获取三件事，加上失败诊断：

| 接口 | 说明 |
| --- | --- |
| `DynamicLibrary::open(path, flags, ec)` | **加载**（唯一入口）；失败返回 `std::nullopt` 并写入 `ec`，严格默认传 `LoadFlags::none` |
| `unload()` | **卸载**（`open()` 的逆操作）；幂等，返回“本次是否真的卸载了” |
| `find_symbol(name)` | **取符号**：解析符号地址（函数或数据），失败返回 `nullptr` |
| `find_function<Fn>(name)` | **取符号**：解析函数符号并转成函数指针类型，失败返回 `nullptr` |
| `is_open()` | 是否持有已加载的库 |
| `~DynamicLibrary()` / 移动构造 / 移动赋值 | RAII 生命周期；析构等价于 `unload()`，不可拷贝 |
| `last_error_detail()` | 最近一次失败的平台原始诊断（线程局部） |
| `describe_error(ec)` | “分类描述 + 平台诊断”的组合文本，适合直接写日志 |
| `ErrorCode` / `to_string()` / `make_error_code()` | 与平台无关的失败分类，可直接当 `std::error_code` 用 |
| `LoadFlags` / `has_flag()` | 加载行为位掩码（默认严格模式） |

约定：**每个公共接口进入时清空诊断、失败时写入**，所以调用返回后 “`last_error_detail()` 为空” 等价于 “本次调用成功”。
`ec.message()` 只由错误码决定（稳定、可复现），平台细节请用 `describe_error()`。

职责边界：插件实例的创建/销毁、插件注册表、插件生命周期编排都属于宿主层（例如 `PluginFactory`），
本库不提供、也不应该提供这类接口 —— 它只把“库”管好，把“插件”留给宿主。

### 卸载语义

`unload()` 释放的是**本对象这一次加载的所有权**，与底层 `dlclose` / `FreeLibrary` 的引用计数一致：

| 场景 | `unload()` 之后 |
| --- | --- |
| 只有本对象持有该库 | 模块真正从进程地址空间卸载（可核对 `/proc/self/maps`） |
| 同一文件被打开了两次（两个 `DynamicLibrary`） | 只是本对象的引用被释放，另一个对象仍可正常解析符号 |
| 其它已加载模块依赖它 | 模块继续驻留（平台的依赖保护） |
| 用 `LoadFlags::keep_loaded` 加载 | 模块按该标志的语义驻留到进程结束 |

三条硬性约束：

1. **卸载后，从该库解析出来的函数指针与数据指针全部失效**，不得再调用或访问；
2. 宿主必须先结束插件对象（先 `shutdown()`、再销毁实例），最后才卸载库，否则虚表指针会变成悬垂指针；
3. 返回值 `false` 只表示“本来就没有可卸载的库”或“卸载失败”，具体原因看 `last_error_detail()`；
   无论成败，对象都会回到“未打开”状态，不会留下不可用的句柄。

推荐的插件卸载顺序（宿主视角）：

```cpp
plugin->shutdown();          // 1. 让插件自己收尾
plugin_destroy(plugin);      // 2. 销毁插件对象（此后不得再触碰它的虚表）
library->unload();           // 3. 最后卸载库
```

---

## 3. 平台矩阵

### 加载标志

| `LoadFlags` | Linux (glibc) | Windows |
| --- | --- | --- |
| `none`（默认） | `RTLD_NOW` + `RTLD_LOCAL` | `LoadLibraryExW` + 严格搜索路径 |
| `lazy_binding` | `RTLD_LAZY` | 无对应能力，忽略（导入表在加载时解析） |
| `global_visibility` | `RTLD_GLOBAL` | 无对应能力，忽略（符号默认进程可见） |
| `deep_binding` | `RTLD_DEEPBIND` | 无对应能力，忽略 |
| `keep_loaded` | `RTLD_NODELETE` | `GetModuleHandleExW(PIN)` 固定模块；失败则整体 `open` 失败 |

“无对应能力”是**明确记录在案**的行为，不是静默丢弃：表里没有的能力，后端不会伪造语义。

### 失败分类

| `ErrorCode` | Linux 触发条件 | Windows 触发条件 |
| --- | --- | --- |
| `invalid_argument` | 空路径 / 无法解析为绝对路径 | 同左 |
| `file_not_found` | 文件不存在；`cannot open shared object file` | 文件不存在；`ERROR_FILE_NOT_FOUND` / `PATH_NOT_FOUND` / `MOD_NOT_FOUND` |
| `not_a_library` | `invalid ELF header` / `wrong ELF class` / `file too short` | `ERROR_BAD_EXE_FORMAT` / `EXE_MACHINE_TYPE_MISMATCH` / `DLL_INIT_FAILED` |
| `permission_denied` | `Permission denied` | `ERROR_ACCESS_DENIED` |
| `symbol_not_found` | `dlsym` 后 `dlerror()` 有值 | `GetProcAddress` 返回空 |
| `out_of_memory` | `cannot allocate memory` | `ERROR_NOT_ENOUGH_MEMORY` / `ERROR_OUTOFMEMORY` |
| `unknown` | 其它 `dlerror()` 文本 | 其它 Win32 错误码 |

分类是“尽力而为”的字符串/错误码映射；**无论分类是否命中，平台原始文本都完整保留在 `last_error_detail()` 里**。

---

## 4. 严格模式体现在哪里

| 方面 | 做法 | 理由 |
| --- | --- | --- |
| 绑定时机 | 默认 `RTLD_NOW`（立即绑定） | 坏库在 `open()` 就失败，而不是第一次调用时崩溃 |
| 符号可见性 | 默认 `RTLD_LOCAL` | 插件不污染进程全局符号空间 |
| 路径解析 | 先转绝对路径再交给平台加载器 | 避免“当前目录 / 搜索路径”命中同名库 |
| Windows 搜索路径 | `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR \| LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` | 防 DLL 劫持；老系统自动回退 `LOAD_WITH_ALTERED_SEARCH_PATH` |
| 失败可见性 | 不抛异常、不吞错误；`ec` + `last_error_detail()` 双通道 | “成功了但不知道为什么没生效” 这类问题不会出现 |
| 明确要求的能力 | 例如 Windows 的 `keep_loaded` 固定失败 → 整体加载失败 | 不静默忽略调用方明确请求的能力 |
| 异常边界 | 所有 `noexcept` 接口内部兜住 `bad_alloc` / `std::exception` / `...` | 宿主进程不会因为加载失败而 `std::terminate()` |
| 句柄泄漏 | 构造期间用 `HandleGuard`（RAII）托管原生句柄 | `open()` 的任何失败路径都不泄漏 |
| 加载/卸载对称 | `unload()` 是 `open()` 的逆操作且返回“是否真的卸载了” | 加载与卸载成对出现，语义一目了然 |

---

## 5. 如何新增一个平台后端（开闭原则）

**扩展点就是后端**：新增平台不需要修改任何既有代码，只需两步。

1. 新建 `src/<平台>/dynamic_library_<平台>.cpp`（**不需要** CMakeLists）：
   - `.cpp` 里定义 `struct DynamicLibrary::Impl`，并实现下面这组符号（就是后端与公共头之间的全部契约）：
     ```
     DynamicLibrary::DynamicLibrary() noexcept
     DynamicLibrary::~DynamicLibrary()
     DynamicLibrary::DynamicLibrary(DynamicLibrary&&) noexcept
     DynamicLibrary& DynamicLibrary::operator=(DynamicLibrary&&) noexcept
     DynamicLibrary::open(path, LoadFlags, std::error_code&)
     DynamicLibrary::unload()
     DynamicLibrary::find_symbol(std::string_view)
     dll_loader::last_error_detail()
     ```
     （`is_open()` / `find_function<Fn>()` 已在头文件内联，后端不需要重复实现；
     公开 API 只有“加载 / 卸载 / 取符号 + 错误诊断”，后端也不要新增别的方法。）
     每个接口进入时清空诊断、失败时写入；`open()` 必须在构造期间用 RAII 守卫兜住原生句柄。
2. 在 `DLLLoder/CMakeLists.txt` 的**平台选择表**里加两段 —— 探测表登记后端名，后端表登记源文件与该平台自己的编译要求：
   ```cmake
   # 探测表：目标平台 → 后端名
   elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
       set(DLLLODER_BACKEND_EFFECTIVE freebsd)

   # 后端表：后端名 → 源文件 + 编译要求
   elseif(DLLLODER_BACKEND_EFFECTIVE STREQUAL "freebsd")
       set(DLLLODER_BACKEND_SOURCE "src/freebsd/dynamic_library_freebsd.cpp")
       set(DLLLODER_BACKEND_DEFINITIONS "")
       set(DLLLODER_BACKEND_LIBRARIES "${CMAKE_DL_LIBS}")
   ```
   （可选：把 `freebsd` 追加到 `DLLLODER_BACKEND` 的 `STRINGS` 列表，配置界面会给出提示。）

公共头文件、既有后端、测试都不需要改动 —— 只动这一个 CMake 文件里的平台表。也可以用 `-DDLLLODER_BACKEND=<平台>` 在配置时手工指定后端（交叉编译或调试新后端时有用）；未登记的平台上配置会直接失败并给出上述指引，而不是产出一个“看起来能编译、实际跑不了”的库。

---

## 6. 构建与测试

作为根工程的一部分（`APlugin/CMakeLists.txt` 里 `add_subdirectory(DLLLoder)`）：

```bash
cmake -S . -B build
cmake --build build --target dll_loader_tests
ctest --test-dir build --output-on-failure
```

严格验证（告警即错误 + ASan/UBSan）：

```bash
cmake -S . -B build-strict -DCMAKE_BUILD_TYPE=Debug \
      -DDLLLODER_WARNINGS_AS_ERRORS=ON -DDLLLODER_ENABLE_SANITIZERS=ON
cmake --build build-strict --target dll_loader_tests
ctest --test-dir build-strict --output-on-failure
```

子项目选项：

| 选项 | 默认 | 说明 |
| --- | --- | --- |
| `DLLLODER_BUILD_TESTS` | 跟随 `BUILD_TESTING` | 关闭后不构建样例库与测试 |
| `DLLLODER_WARNINGS_AS_ERRORS` | `OFF` | `-Wall -Wextra -Wpedantic -Wconversion …` 之上再加 `-Werror` / `/WX` |
| `DLLLODER_ENABLE_SANITIZERS` | `OFF` | ASan + UBSan（GCC/Clang），MSVC 用 `/fsanitize=address` |
| `DLLLODER_BACKEND` | `auto` | `auto` / `windows` / `linux` |

测试覆盖：未打开句柄语义、空路径 / 文件不存在 / 非动态库文件三类失败、函数与数据符号解析、缺失符号诊断、卸载幂等、卸载后重新加载、移动构造与移动赋值、同一文件两个句柄各自独立卸载、`LoadFlags` 组合、`std::error_code` 互操作。

---

## 7. 设计决策与注意事项

**为什么一个平台一份 cpp，而不是一份 cpp 里写 `#if`？**
平台差异（接口、标志、错误码、诊断文本）是**策略差异**而不是零星语句差异。拆成独立文件后：每个后端的编译要求可以单独声明（Linux 需要 `_GNU_SOURCE` 与 `libdl`，Windows 不需要）；平台代码 100% 可读、可单独 review；新增平台不会把 `#if` 迷宫扩散到既有代码。代价是少量样板代码重复（生命周期与句柄管理那二十来行），这是有意识的取舍。

**为什么接口不抛异常？**
加载失败在插件宿主里是**常规控制流**（插件缺失、架构不符、符号不全），不是异常情况；统一用 `std::error_code` 让“成功/失败”必须被显式处理，也避免异常穿过 ABI 边界。整库没有任何一处抛出异常，风格不混用。

**为什么用 pimpl？**
`DynamicLibrary` 的私有成员 `Impl` 由各平台自定义，因此**新增平台永远不需要改公共头文件**；头文件里连原生句柄类型都不会泄漏。

**线程安全**
不同 `DynamicLibrary` 对象可以在不同线程并发使用；`find_symbol()` / `find_function()` 是 `const` 且无内部缓存，可以并发调用；同一个对象上的 `unload()` 与其它调用需要调用方自行同步；`last_error_detail()` 是线程局部的。

**已知约束**
- Linux 的 `RTLD_DEEPBIND`（`LoadFlags::deep_binding`）与 AddressSanitizer / UBSan 运行时不兼容，sanitizer 构建下请勿使用该标志（测试在 sanitizer 构建中会自动跳过它）。
- `find_function<Fn>()` 无法校验跨 ABI 的签名一致性：函数指针类型由调用方负责与导出符号的真实签名匹配（这是 C++ 类型系统管不到的部分，`extern "C"` 导出头是唯一的约定来源）。
- 符号恰好位于地址 0 时，POSIX 的 `dlsym` 无法与“失败”区分，本库按更严格的“未找到”处理。
- `open()` 会先检查路径是否存在，因此“依赖项缺失”也会表现为 `not_a_library` 或 `file_not_found`，具体请看 `describe_error()` 的文本。
