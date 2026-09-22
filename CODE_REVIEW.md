# 插件系统 Code Review 报告

- **评审对象**：插件管理宿主层（扫描 / 加载 / 卸载）与配套的插件样例、构建脚本
- **核心文件**：`PluginManager/plugin_factory.h`、`PluginManager/plugin_factory.cpp`、`PluginManager/utils/utils.h`、`PluginManager/utils/utils.cpp`、`PluginManager/metainfo.h`、`PluginManager/common/plugin-api/plugin_api.h`、`plugin_test/plugin_a/*`
- **基线**：`844876c`（评审过程中作者已把 `lib*.so` 回退逻辑的修复 amend 进该提交，评审工作区干净，无未提交改动）
- **评审方式**：源码通读 + 真实编译（GCC 13.3 / C++20 / `-Wall -Wextra -Wpedantic` 全开）+ 可执行最小复现（含 ASan/LSan）+ cppcheck
- **结论**：**Request Changes（不建议合并）**。当前状态下「按名字加载插件」这条主路径**在功能上完全不可用**，且存在 3 处必然崩溃、1 处必然内存泄漏。

---

## 1. 总体评价

设计意图（一句话）：宿主扫描插件目录下的 `metainfo.json`，按注册名加载对应的 `插件名.so`，并以 `shutdown → destroy → unload` 的顺序安全卸载。

底层的 `dll_loader`（`PluginManager/common/DLLLoder/`）质量很高：RAII、`noexcept` 边界、`std::error_code` 分类、平台后端隔离、17 个单元测试，README 甚至明确写出了宿主的正确卸载顺序。**但宿主层 `PluginFactory` 没有继承这套纪律**——它用 `std::cerr` 代替错误传递、用裸指针代替句柄、用「打印一行日志」代替校验，并且在几处关键分支上写反了判断条件，导致上面那句设计意图目前无法成立。

| 级别 | 数量 | 概要 |
| --- | --- | --- |
| 🔴 Critical | 7 | 名字比较取反导致插件永远加载不了；空指针解引用崩溃 ×2；`metainfo.json` 缺字段直接 `terminate`；二次卸载 UAF；插件实例永不析构；样例插件缺少导出符号；公开头文件泄漏 `dll_loader` 依赖导致下游编译失败 |
| 🟠 Major | 8 | 重复加载泄漏；`initialize()` 返回值被忽略；卸载不清理表项；元信息校验形同虚设；硬编码 `.so`/`/`；Windows 插件无法编译；无测试/无宿主样例；API 无法区分失败原因 |
| 🟡 Minor | 9 | 扫描深度语义、头文件命名空间污染、缺失 include、`operator[]` 静默插入、日志无插件名、遍历拷贝、可见性/告警开关未接入等 |

---

## 2. 验证环境与复现结果

编译环境：`g++ (Ubuntu 13.3.0) / cmake 3.28.3 / C++20`，`cmake -S . -B build-review && cmake --build build-review` 构建通过，严格告警集（`-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast -Wnull-dereference …`）下**无告警**，cppcheck（`warning,style,performance,portability`）**无告警**——也就是说，下面所有问题都不是编译器/静态分析能替你发现的，必须靠语义审查与运行验证。

我用一个自建插件（`good.so`，正确导出 `plugin_create` / `plugin_destroy` / `plugin_get_meta_info`）直接调用宿主层 API 复现，结果如下：

| 场景 | 期望 | 实测 | 证据 |
| --- | --- | --- | --- |
| `scanFileFolder(dir)` + `loadPlugin("good")`（目录内 `metainfo.json` 的 `name` 就是 `"good"`） | 返回有效实例 | **返回 `nullptr`，且没有任何错误输出** | `loadPlugin("good") 返回 = (nil)` |
| `loadPlugin("nope")`（不存在的名字） | 返回 `nullptr` | **段错误（SIGSEGV, exit 139）** | 对 `nullptr` 调用 `initialize()` |
| 目录内 `metainfo.json` 缺少 `api_version` | 跳过该插件 | **`terminate`（exit 134）** | `nlohmann::json::out_of_range` 逃逸出 `scanFileFolder` |
| `metainfo.json` 的 `name` 与 `.so` 文件名**不一致**时 `loadPlugin("alias")` | 拒绝加载 | **成功加载**（唯一能走通的路径） | `[alias] ctor / initialize`，返回非空指针 |
| 加载后 `PluginFactory` 析构 | 实例被 `shutdown + destroy` | **实例既不析构也不销毁** | 无 `DTOR` 输出；LSan 报 `Direct leak of 8 byte(s)`，栈顶为 `plugin_factory.cpp:105` 的 `fn_plugin_create()` |
| 同一指针 `unloadPlugin` 两次 | 安全返回 | **段错误（exit 139）** | 第二次对已 `dlclose` 的模块虚表发起调用（UAF） |
| 连续两次 `loadPlugin("alias")` | 拒绝或复用 | 产生 2 个实例，0 次销毁 | `[alias] ctor` ×2，`plugin_destroy` 从未调用 |
| `initialize()` 返回 `false` | 加载失败 | **照常返回"可用"实例** | `initialize() 返回 false，但 loadPlugin 仍返回 = 0x…` |
| `plugin_create()` 返回 `nullptr` | 加载失败 | **段错误（exit 139）** | 同 `nope` 场景，`initialize()` 空指针调用 |
| 仅按 `plugin_manager` 的公开使用要求编译下游代码 | 编译通过 | **编译失败** | `fatal error: dll_loader/dynamic_library.hpp: 没有那个文件或目录` |
| `nm -D plugin_a.so` | 导出三个入口 | **只导出了 `plugin_get_meta_info`** | 缺少 `plugin_create` / `plugin_destroy` |

---

## 3. 🔴 Critical（必须修复，阻断合并）

### 已修复
### C1. `loadPlugin` 的名字比较写反，注册名插件永远加载不出来

`PluginManager/plugin_factory.cpp:28-41`

```cpp
for(auto [key, value] : m_metainfos)
{
    if (value.name.compare(name)) {   // ★ std::string::compare 相等返回 0
        auto plugin = load(key, name);  // 只在“名字不相等”时才去加载
        ...
```

`compare()` 返回 0 表示相等，`if (0)` 为假。因此：**请求的名字与注册名一致时被跳过，不一致时反而进入加载分支**。这就是上表第一行「按名字加载返回 `nullptr` 且毫无提示」的原因，也是第四行「名字不匹配才能加载成功」的原因——两个现象互为佐证，指向同一处笔误。

```cpp
if (value.name.compare(name) == 0)   // 或者更直白： value.name == name
```

> 注意：`m_metainfos` 是 `std::unordered_map`，一旦目录里存在多个插件，这个反向条件还会让"加载到哪个插件"取决于哈希顺序——即行为不确定。

### 已修复
### C2. `load()` 失败后立刻解引用空指针 → 必然崩溃

`PluginManager/plugin_factory.cpp:32-33`

```cpp
auto plugin = load(key, name);   // load() 失败时返回 nullptr（见 :77、:103 等分支）
plugin->initialize();            // ★ 无条件解引用
return plugin;
```

`load()` 的五条失败路径都 `return nullptr`，而调用方一次都没有判空。复现：`loadPlugin("nope")` → SIGSEGV。同理，`plugin_create()` 返回 `nullptr`（插件实现有 bug 时完全可能）也会走到同一个崩溃点（`plugin_factory.cpp:105` → `:33`）。

**修复**：`load()` 返回空即失败返回；`initialize()` 前必须判空。

```cpp
IPlugin* plugin = load(key, name);
if (plugin == nullptr) {
    return nullptr;                       // 失败原因由 load() 内部输出/回传
}
if (!plugin->initialize()) {              // 见 C2' 与 M2
    destroy_and_unload(key);              // 初始化失败必须按 shutdown→destroy→unload 回收
    return nullptr;
}
return plugin;
```

### 已修复
### C3. `metainfo.json` 缺字段时异常逃逸出 `scanFileFolder`（宿主直接终止）

`PluginManager/utils/utils.cpp:19-37`

```cpp
try {
    j = json::parse(ifs);          // ★ try 只包住了 parse
} catch (...) { ... }

return PluginMeta {
    .api_version = j.at("api_version"),   // ★ at() 在 try 之外：键缺失 → json::out_of_range
    .name = j.value("name", ""),
    ...
};
```

`j.at("api_version")` 的 `out_of_range`（以及值类型不对时的 `type_error`）没有任何 catch；`find_metainfo()` 也不 catch；`scanFileFolder()` 也不 catch。实测结果：`terminate called after throwing an instance of 'nlohmann::json_abi_v3_12_0::detail::out_of_range'`，进程 abort（exit 134）。

**这是"一个格式错误的第三方插件文件 → 宿主进程崩溃"的拒绝服务**：插件目录里的 `metainfo.json` 属于外部输入，必须当作不可信数据处理。

**修复**（二选一，建议都做）：

```cpp
// 1) 字段解析也放进 try，且先校验必填项
try {
    j = json::parse(ifs);
    const auto api_version = j.at("api_version").get<std::uint32_t>();   // 必填
    if (api_version == 0) return std::nullopt;                            // 非法值
    return PluginMeta{ .api_version = api_version, .name = j.value("name", ""), ... };
} catch (const json::exception& e) {
    std::cerr << "metainfo 解析失败 " << path << ": " << e.what() << '\n';
    return std::nullopt;
}

// 2) find_metainfo 内调用 Parse 处再兜一层，绝不让插件数据打断扫描
```
### 已修复
### C4. `unloadPlugin` 不清理 `m_dlls`，二次卸载 = 释放后使用（UAF）

`PluginManager/plugin_factory.cpp:44-59`

```cpp
for( auto& [path , dll] : m_dlls) {
    if (dll.plugin == plugin) {
        dll.fn_plugin_destory(plugin);   // 实例已销毁
        dll.dll.unload();                // 模块已卸载：虚表/函数指针全部失效
        break;                           // ★ 表项保留了悬垂的 plugin 与 fn_plugin_destory
    }
}
```

卸载完成后，`m_dlls` 里仍然留着一条 `plugin` 指向已释放对象、`fn_plugin_destory` 指向已卸载代码段的表项。任何一次重复卸载（上层重复调用、异常路径重试）都会执行 `plugin->shutdown()`（`plugin_factory.cpp:48`），即对已 `dlclose` 的虚表发起调用——实测 SIGSEGV。

另外两处同源问题：

- `plugin->shutdown()` 在**查找归属之前**调用：传入一个不属于本工厂的指针，会直接对陌生对象调虚函数；
- 找不到归属时**静默返回**，实例既不销毁也不卸载。

**修复**（必须把表当作"加载态"的唯一真相，卸载 = 移除表项）：

```cpp
void PluginFactory::unloadPlugin(IPlugin* plugin)
{
    if (plugin == nullptr) return;
    auto it = std::find_if(m_dlls.begin(), m_dlls.end(),
                           [plugin](const auto& kv) { return kv.second.plugin == plugin; });
    if (it == m_dlls.end()) {                 // 陌生指针：不触碰它的虚表
        std::cerr << "unloadPlugin: 未知插件指针\n";
        return;
    }
    it->second.plugin->shutdown();
    it->second.fn_plugin_destory(it->second.plugin);
    it->second.dll.unload();
    m_dlls.erase(it);                         // ★ 关键：消除悬垂条目
}
```

### 已修复
### C5. `PluginFactory` 没有析构函数，插件实例永不销毁（已由 LSan 确认泄漏）

`PluginManager/plugin_factory.h:24-43`：类里只有两个容器，没有析构函数。工厂析构时，`m_dlls` 的 `Dll` 成员被销毁，`DynamicLibrary::~DynamicLibrary()` 会 `dlclose` 模块——**但 `IPlugin` 实例从头到尾没有被 `plugin_destroy` 过**，插件对象的析构函数永远不会执行。

这直接违反本仓库自己写下的契约（`PluginManager/common/DLLLoder/README.md:117`）：

> 宿主必须先结束插件对象（先 `shutdown()`、再销毁实例），最后才卸载库，否则虚表指针会变成悬垂指针。

后果：插件持有的文件句柄、线程、GPU 资源、日志缓冲全部泄漏；LSan 输出 `SUMMARY: AddressSanitizer: 8 byte(s) leaked in 1 allocation(s)`，栈帧 `#2 plugin::PluginFactory::load(...) plugin_factory.cpp:105`。

```cpp
~PluginFactory() { unloadAll(); }

void PluginFactory::unloadAll() noexcept
{
    for (auto& [path, dll] : m_dlls) {
        if (dll.plugin != nullptr) {
            try { dll.plugin->shutdown(); } catch (...) {}   // 析构路径不得抛出
            if (dll.fn_plugin_destory) dll.fn_plugin_destory(dll.plugin);
            dll.plugin = nullptr;
        }
        dll.dll.unload();
    }
    m_dlls.clear();
}
```
### 已修复
### C6. 样例插件 `plugin_a` 根本没有导出 `plugin_create` / `plugin_destroy`

`plugin_test/plugin_a/plugin_a.cpp` 只定义了 `plugin_get_meta_info()`。`PluginManager/common/plugin-api/plugin_api.h:57-58` 声明的另外两个入口在样例里不存在，链接共享库时也**不会报错**（Linux 默认允许未定义符号），于是：

```console
$ nm -D --defined-only build/plugin_test/plugin_a/plugin_a.so | grep plugin_create
（无输出）
```

宿主的 `load()` 会在这里失败（`plugin_factory.cpp:95-99`）。换句话说，**仓库里唯一的样例插件是不可加载的**，这也解释了为什么这套功能"看起来能跑"却没有端到端验证。请补齐：

```cpp
extern "C" {
PLUGIN_API plugin::IPlugin* plugin_create() { return new Plugin_A(); }
PLUGIN_API void plugin_destroy(plugin::IPlugin* p) { delete p; }
}
```

并建议：插件 target 加 `-Wl,--no-undefined` / 导出符号白名单，或在 CI 里断言三个入口符号存在，让"插件忘了写导出"在构建期就暴露。

### 已修复
### C7. 公开头文件引用 `dll_loader`，但 CMake 里它是 `PRIVATE` → 下游无法编译

`PluginManager/plugin_factory.h:4,20` 在**公开头文件**里 `#include <dll_loader/dynamic_library.hpp>` 并按值持有 `dll_loader::DynamicLibrary`；而 `PluginManager/CMakeLists.txt:20-27` 把 `dll_loader` 列为 `PRIVATE`。实测下游只带 `plugin_manager` 的使用要求编译：

```
fatal error: dll_loader/dynamic_library.hpp: 没有那个文件或目录
```

CMakeLists 里的注释其实写明了这条规则（"任何对外头文件都不得引用 nlohmann/json，否则必须改为 PUBLIC"）——只是 `dll_loader` 被漏掉了。修复二选一：把 `dll_loader` 改为 `PUBLIC`，或把 `Dll` 从公开头文件里挪走（pimpl / 只暴露不透明句柄）。

**顺带**：`plugin_manager` 目前也**没有接入** `dllloder_set_warnings()`，宿主层实际是在无严格告警下编译的。

---

## 4. 🟠 Major（应当修复）

**M1. 同一个插件重复加载会泄漏前一个实例。** `plugin_factory.cpp:106` 用 `m_dlls[plugin_folder] = {...}` 直接覆盖：同一目录第二次加载会新建实例、覆写表项，第一个实例的 `plugin` 指针与 `fn_plugin_destory` 就此丢失（复现：`[alias] ctor` ×2、`plugin_destroy` 0 次）。加载前应检查 `m_dlls.contains(folder)`：返回同一实例（引用计数）或明确拒绝。

**M2. `initialize()` 的 `bool` 返回值被丢弃。** `plugin_factory.cpp:33` 无视失败结果，把一个"初始化失败"的插件当作可用对象交给调用方（已复现）。插件 API 既然定义了 `bool initialize()`，宿主就必须检查并走回收路径。

**M3. 元信息校验形同虚设。** `plugin_factory.cpp:90-92`：不匹配时只 `std::clog` 一行就继续加载，且消息里不含插件名；同时 `fn_plugin_meta()` 的返回值**没有判空**就直接传给 `metainfo_compare`（`:89`），返回 `nullptr` 即 UB。另外 `utils.cpp:42-49` 只比较 3 个字段、`code_info->version` 是 `const char*` 也可能为 `nullptr`。样例自身就不自洽：`plugin_a.cpp` 写 `version = "1.0.0"`，`metainfo.json` 写 `"1.0"` —— 现在这条校验是"哑的"，所以没人发现。

**M4. 完全没有 `api_version` 兼容性校验。** `plugin_api.h:30` 注明"必须与宿主匹配"，但宿主没有任何 `HOST_API_VERSION` 常量，也没有任何比较或拒绝逻辑（全局 grep 只有插件侧赋值）。跨版本 ABI 不兼容的插件会被静默加载并在调用时崩溃。建议：宿主定义 `constexpr std::uint32_t kPluginApiVersion = 1;`，不匹配即拒绝加载。

**M5. 硬编码 `.so` 与 `/`，Windows 后端形同虚设。** `plugin_factory.cpp:64,72`：`std::format("{}/{}.so", ...)`。仓库明明带了 `src/windows/dynamic_library_windows.cpp`，但 `load()` 永远拼不出 `.dll` 路径。应使用 `fs::path` 与平台后缀：

```cpp
#if defined(_WIN32)
constexpr std::string_view kPluginExt = ".dll";
#else
constexpr std::string_view kPluginExt = ".so";
#endif
const fs::path fullpath = plugin_folder / (std::string{name} + std::string{kPluginExt});
```

**M6. Windows 上插件根本无法编译。** 没有任何 target 定义 `PLUGIN_BUILD_SHARED`（全仓库 grep 只有 `plugin_api.h` 里的 `#ifdef`）。于是在 Windows 上 `PLUGIN_API` 退化为 `__declspec(dllimport)`，而插件源文件正在**定义**这些函数 → MSVC `C2491: definition of dllimport function not allowed`。修复：插件 target 加 `target_compile_definitions(${PLUGIN_NAME} PRIVATE PLUGIN_BUILD_SHARED)`。

**M7. 宿主层没有任何测试，`example/` 是空的。** `example/CMakeLists.txt` 为 0 字节，`plugin_test/plugin_a` 是插件不是测试；`plugin_manager` 没有一条 CTest 用例。底层 `dll_loader` 有 17 个测试，宿主层 0 个——C1~C6 全部是"一个 5 行的端到端测试就能抓到"的问题。建议补 `tests/test_plugin_factory.cpp`：正常加载、名字不存在、`metainfo.json` 损坏、重复加载、二次卸载、析构回收。

**M8. 失败原因无法被程序感知。** 所有错误路径都是 `std::cerr` + `return nullptr`，"插件不存在"和"动态库加载失败""符号缺失""元信息不匹配"对调用方完全同构。这层 API 与下层 `dll_loader` 的 `std::error_code` + `describe_error()` 风格完全脱节（下游无法重试、无法降级、无法做 UI 提示）。建议 `loadPlugin` 返回 `std::expected<IPlugin*, std::error_code>`（或出参 `std::error_code&`），日志只作为附加诊断。

---

## 5. 🟡 Minor（建议改进）

1. **`find_metainfo` 的 `max_depth` 语义与注释不符（实测差一层）。** `utils.cpp:85` 的 `it.depth() + 1 >= max_depth` 让默认 `max_depth = 2` 只能发现 `root/<插件目录>/metainfo.json`；`root/a/b/metainfo.json` 实测**扫描不到**（返回 `nil`），而函数注释写的是"递归扫描插件目录下的 meta info"。建议改为 `it.depth() >= max_depth` 并把语义写进注释，或去掉该参数。
2. **`utils.cpp:69-73` 的错误处理存在死循环风险。** `for (; it != end; it.increment(ec))` 中 `increment` 失败后 `ec.clear(); continue;` —— 若实现未把迭代器推进到 `end`，将持续重试同一条目。建议改为 `while` + 显式判断 `it == end` 后 `break`。
3. **目录不存在时全程静默。** `find_metainfo` 对非目录返回空表（`utils.cpp:58`），`scanFileFolder` 不校验、不报错，随后 `loadPlugin` 又静默返回 `nullptr`。建议在 `scanFileFolder` 里对目录合法性给出明确诊断。
4. **`utils.h` 缺少自包含的 include。** 使用了 `std::optional`、`std::unordered_map` 却未 `#include <optional>` / `<unordered_map>`（当前靠 `json.hpp` 传递包含，换头文件即碎）；`<filesystem>` 重复包含两次（`utils.h:2,6`）。
5. **`plugin_factory.h:10-11` 在全局命名空间定义 `namespace fs = std::filesystem;` 与 `using PluginPath = fs::path;`。** 公开头文件污染全局命名空间，且与使用者自己的 `fs` 别名冲突。建议移入 `plugin::detail` 或直接用 `std::filesystem::path`。
6. **`m_metainfos[plugin_folder]` 用了 `operator[]`（`plugin_factory.cpp:90`）。** 键不存在时会静默插入默认 `PluginMeta` 再拿去比较，掩盖调用错误；应用 `.at()` 或 `find()`。
7. **日志质量。** `std::cerr << ec.what()`（`:36`）无换行、无插件名、无路径；`STRINGIFY(plugin_create)`（`:83,95,100`）为了写 `"plugin_create"` 引入了两个宏，直接写字符串字面量更清晰，也能避免 `STRINGIFY` 污染公共头文件。
8. **遍历与查找效率。** `plugin_factory.cpp:28` 的 `for (auto [key, value] : m_metainfos)` 按值拷贝（`fs::path` + 4 个 `std::string`）；按名字加载是线性扫描 + `unordered_map` 无序。建议增加 `std::unordered_map<std::string, fs::path> m_index_by_name`；卸载按键（或用 RAII 句柄）而非线性找指针。
9. **`Dll` 的 `fn_plugin_meta` 从未被赋值**（`:106-111` 跳过了它），元信息函数指针在加载后即丢失，后续做版本校验/热重载还得重新 `dlsym`；`Dll` 里 `IPlugin*` 与 RAII 的 `DynamicLibrary` 混放，所有权语义不清。
10. **可见性与构建细节。** Linux 上未设 `CXX_VISIBILITY_PRESET hidden`，`nm -D` 显示 `plugin_a.so` 把 `Plugin_A` 的方法、`_ZTV`/`_ZTI` 全部导出，多插件共存时存在符号冲突/意外插桩风险；顶层 `CMakeLists.txt` 未设 `CMAKE_CXX_EXTENSIONS OFF`（实际是 `gnu++20`）；`plugin_api` 作为 INTERFACE 库未声明 `cxx_std_20`，脱离本仓库顶层构建时会踩坑。安全上建议在文档中明确"插件目录必须不可被非特权用户写入"，并保持当前的 `RTLD_NOW` + `RTLD_LOCAL` 默认值（这一点做得对）。

---

## 6. ✅ 值得肯定

- **`dll_loader` 的分层非常干净**：公共头零平台代码，平台差异收敛在 `src/<平台>/` 与 CMake 的"平台选择表"；`HandleGuard` 保证 `open()` 中途失败不泄漏句柄；`noexcept` 边界用三个 catch 兜底（`dynamic_library_linux.cpp:196-209`）；`ErrorCode` 接入 `std::error_category`；README 把平台矩阵、卸载语义、新增后端的步骤写得比多数商业库清楚。
- **插件 ABI 设计方向正确**：`extern "C"` 入口 + `plugin_create` / `plugin_destroy` 函数指针（`plugin_api.h:52-59`），避开了跨模块 `new`/`delete` 与 C++ 异常穿越 ABI 的经典坑；`PluginMetaInfo` 用 `const char*` 而非 `std::string`，是稳定的 POD 描述；`IPlugin` 纯虚 + 虚析构。
- **`DynamicLibrary` 是教科书式的值类型**：move-only、`unload()` 幂等、析构即卸载、`[[nodiscard]]` 齐全，`find_function` 用 concept 约束 `FunctionPointer` 并把唯一的 `reinterpret_cast` 收在一处。
- **`utils.cpp:52-91` 的目录遍历**用了 `std::error_code` 重载（不抛异常）+ `skip_permission_denied`（单个条目失败不中断整体），说明作者对系统编程的失败模式有意识——只是 `Parse` 自己破了这条纪律（C3）。
- 构建布局（INTERFACE 库 + 别名 target + 插件与 `metainfo.json` 安装到同一目录）已经为后续分发打好了基础。

---

## 7. ❓ 需要作者确认

1. `metainfo.json` 与插件代码里的元信息**不一致**时，期望行为是"拒绝加载"还是"以 json 为准并告警"？当前是"告警后继续"（且样例就处于不一致状态）。
2. `m_metainfos` / `m_dlls` **以插件目录为键**，意味着"一个目录只能放一个插件"。这是有意的约束吗？若是，建议在 `scanFileFolder` 里校验并报错（同目录多个 `metainfo.json`、或 `.so` 名与注册名不符）。
3. `scanFileFolder` 是否会被**重复调用**（热重载/刷新）？当前会整体替换 `m_metainfos` 但不动 `m_dlls`，已加载插件会与新表脱节。
4. 宿主是否需要**多线程**调用（例如工作线程加载、UI 线程卸载）？当前无任何同步原语，也未在注释里声明单线程约束。
5. 插件目录是否可能包含**不可信内容**（用户可写的目录 / 下载的第三方插件）？若是，需要把"加载即执行任意代码"的信任边界写进文档，并考虑校验（哈希/签名）。
6. 是否计划支持 Windows？若是，`plugin_factory.cpp` 的路径拼接（M5）与 `PLUGIN_BUILD_SHARED`（M6）需要一并处理。

---

## 8. 建议的修复顺序

1. **C1 + C2**（两行级修复）→ 让"按名字加载"这条主路径真正可用；
2. **C6 + M7**（补齐样例导出 + 一个端到端 CTest）→ 让上述修复有回归保护；
3. **C4 + C5 + M1**（卸载/析构/重复加载统一走"表项即所有权"模型）→ 消除 UAF 与泄漏；
4. **C3 + M3 + M4**（把 `metainfo.json` 当外部输入：字段校验、异常兜底、`api_version` 校验）；
5. **C7 + M5 + M6**（构建与跨平台：`dll_loader` 可见性、路径后缀、`PLUGIN_BUILD_SHARED`、可见性预设、接入 `dllloder_set_warnings`）；
6. **M8 + Minor**（把 `std::error_code` 贯通到宿主 API，API 收敛为 RAII 句柄）。

> 复现环境说明：本报告的所有实测结论来自 GCC 13.3 + C++20 + ASan/LSan 的真实执行；复现用的临时构建目录（`build-review/`，已被 `.gitignore` 的 `build-*/` 覆盖）已清理，仓库源码未被修改。
