#include "plugin_factory.h"
#include <algorithm>
#include <dll_loader/dynamic_library.hpp>
#include "dll_loader/load_error.hpp"
#include "plugin_api.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <format>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <mutex>
#include "chain.hpp"

// 原代码里顺手要修的两个 bug
// loadPlugin 里 load(key, name) 返回后 m_dlls[plugin_folder] 二次查找 → 用返回的 Dll* 直接操作即可，也避免 operator[] 意外默认构造。



namespace plugin {

struct Dll  {
    PluginMetaFunc              fn_plugin_meta    = nullptr;     // 获取插件元信息
    PluginCreateFunc            fn_plugin_create  = nullptr;     // 创建插件
    PluginDestroyFunc           fn_plugin_destory = nullptr;    // 销毁实例
    dll_loader::DynamicLibrary  dll;
    bool                        is_init = false;
    IPlugin*                    plugin = nullptr;
};


struct PluginFactory::impl {
public:
    explicit impl() = default;

    /// 递归扫描插件目录下的 meta info
    void scanFileFolder_unlocked(const std::filesystem::path& plugins_folder);

    IPlugin* loadPlugin_unlocked(std::string_view name);
    bool unloadPlugin_unlocked(IPlugin* plugin);

    IPlugin* getPlugin_unlocked(std::string_view name);  // 获取已经加载过的 plugin

    /// @todo 考虑按依赖逆序销毁
    void unloadAll() noexcept;

    /// @param plugin_folder 必须为插件目录的绝对路径（相对路径未测试）,
    //                       该路径下存放 metainfo.json 与 so 文件
    /// @param name          为 插件 metainfo.json 中注册的插件名，应当与 so 文件名相同
    std::optional<Dll*> load(const std::filesystem::path& plugin_folder, std::string_view name);

public:
    mutable std::shared_mutex m_mutex;   // 锁切面的“点”

    std::filesystem::path m_plugins_path;

    /// 这里的 PluginPath 都必须是指向 so 文件所在文件夹的路径
    std::unordered_map<PluginPath, PluginMeta> m_metainfos;   // metainfo.json 中获取
    std::unordered_map<PluginPath, Dll>        m_dlls;
};


template <typename Mutex, typename LockT>
class LockAspect {
    Mutex& mtx_;
public:
    explicit LockAspect(Mutex& mtx) : mtx_(mtx) {}

    template <typename F>
    auto operator()(F&& f) const {
        return [&mtx = mtx_, f = std::forward<F>(f)](auto&&... args)
            -> decltype(auto) 
        {
            LockT lk(mtx);                        // RAII = 前置/后置通知
            return std::invoke(f, std::forward<decltype(args)>(args)...);
        };
    }
};

template <typename Mutex>
auto lock_aspect(Mutex& mtx) {
    return LockAspect<Mutex, std::unique_lock<Mutex>>(mtx);
}
template <typename Mutex>
auto shared_lock_aspect(Mutex& mtx) {
    return LockAspect<Mutex, std::shared_lock<Mutex>>(mtx);
}



namespace fs = std::filesystem;

PluginFactory::PluginFactory()
    : impl_ptr(std::make_unique<impl>())
{
    
}

PluginFactory::~PluginFactory()
{
    impl_ptr->unloadAll(); 
}

void PluginFactory::scanFileFolder(const fs::path& plugin_folder)
{   
    aspect::chain(
        lock_aspect(impl_ptr->m_mutex)
    )([this, &plugin_folder](){ 
        impl_ptr->scanFileFolder_unlocked(plugin_folder); 
    })();
}

IPlugin* PluginFactory::loadPlugin(std::string_view name)
{
    return aspect::chain(
        lock_aspect(impl_ptr->m_mutex)
    )([&]() -> IPlugin* {
        return impl_ptr->loadPlugin_unlocked(name);
    })();
}

bool PluginFactory::unloadPlugin(IPlugin* plugin)
{
    return aspect::chain(
        lock_aspect(impl_ptr->m_mutex)
    )([&]() {
        return impl_ptr->unloadPlugin_unlocked(plugin);
    })();
}

IPlugin* PluginFactory::getPlugin(std::string_view name)
{
    return  aspect::chain(
        shared_lock_aspect(impl_ptr->m_mutex)
    )([&]() -> IPlugin* {
        return impl_ptr->getPlugin_unlocked(name);
    })();
}


inline void PluginFactory::impl::scanFileFolder_unlocked(const std::filesystem::path& plugins_folder) 
{
    auto info = utils::find_metainfo(plugins_folder);
    for(const auto& [key, val] : info) 
    {
        if (m_metainfos.find(key) == m_metainfos.end()) {
            m_metainfos.insert({key, val});
        }
        std::clog << "plugin: " << val.to_string() << "\n";
    }
}

inline IPlugin* PluginFactory::impl::loadPlugin_unlocked(std::string_view name) 
{
    /// 这里的 key 就是 fs::path 指向 so 文件目录的绝对路径
    for(auto& [key, meta] : m_metainfos) 
    {
        if (meta.name.compare(name) != 0) continue;
        
        try {
            auto dll_opt = load(key, name);
            if (!dll_opt.has_value()) 
                return nullptr;
            auto dll = *dll_opt;
            auto plugin = dll_opt.value()->plugin;
            if (!dll->is_init) 
            {
                if (!plugin->initialize()) {
                    unloadPlugin_unlocked(plugin);              // 初始化失败必须按 shutdown→destroy→unload 回收
                    std::cerr << std::format("插件 {} 初始化失败\n", dll->plugin->name());
                    return nullptr;
                }
                std::clog << std::format("插件 {} 已经初始化\n", dll->plugin->name());
                dll->is_init = true;
                
            } 
            else {
                std::clog << std::format("插件 {} 已经加载过了\n", dll->plugin->name());
            }

            return plugin;
        } catch (std::runtime_error& ec) {
            std::cerr << ec.what() << "\n";
            return nullptr;
        }
        
    }
    return nullptr;    
}

inline bool PluginFactory::impl::unloadPlugin_unlocked(IPlugin* plugin) 
{
    if (plugin == nullptr) return false;
    /// 回收资源
    auto it = std::find_if(
    m_dlls.begin(), 
    m_dlls.end(),
    [plugin](const auto& kv) 
        { 
            return kv.second.plugin == plugin; 
        }
    );

    if (it == m_dlls.end()) {                 // 陌生指针：不触碰它的虚表
        std::cerr << "unloadPlugin: 未知插件指针\n";
        return false;
    }
    it->second.plugin->shutdown();
    it->second.fn_plugin_destory(it->second.plugin);
    it->second.dll.unload();
    // 清理列表
    m_dlls.erase(it);

    return true;
}

inline IPlugin* PluginFactory::impl::getPlugin_unlocked(std::string_view name) {
    for(const auto& [key, dll] : m_dlls) {
        if (dll.plugin->name() == name) {
            return dll.plugin;
        }
    }
    return nullptr;
}

inline void PluginFactory::impl::unloadAll() noexcept {
    for (auto& [path, dll] : m_dlls) {
        if (dll.plugin != nullptr) {
            try { dll.plugin->shutdown(); } catch (...) {}   // 析构路径不得抛出
            if (dll.fn_plugin_destory) 
                dll.fn_plugin_destory(dll.plugin);
            dll.plugin = nullptr;
        }
        dll.dll.unload();
    }
    m_dlls.clear();
}

inline std::optional<Dll*> PluginFactory::impl::load(const std::filesystem::path& plugin_folder, std::string_view name) 
{
    auto it = std::find_if(m_dlls.begin(), m_dlls.end(), [&plugin_folder](const auto& kv) {
        return kv.first == plugin_folder;
    });
    if (it != m_dlls.end()) {
        // 插件被加载过
        return &it->second;
    }


    std::string fullpath = std::format("{}/{}.so", plugin_folder.generic_string(), name);
    
    std::error_code ec;
    auto dll_lib = dll_loader::DynamicLibrary::open(fullpath, dll_loader::LoadFlags::none, ec);
    if (!dll_lib.has_value()) {
        std::cerr << dll_loader::describe_error(ec) << "\n";
        dll_lib.reset();
        std::string fullpath2 = std::format("{}/lib{}.so", plugin_folder.generic_string(), name);
        dll_lib = dll_loader::DynamicLibrary::open(fullpath2, dll_loader::LoadFlags::none, ec);

        if (!dll_lib.has_value()) {
            std::cerr << dll_loader::describe_error(ec) << "\n";
            return std::nullopt;
        }
    }
    
        
    /// 找到 plugin_create 并调用
    auto fn_plugin_meta = dll_lib->find_function<PluginMetaFunc>(STRINGIFY(plugin_get_meta_info));
    if (fn_plugin_meta == nullptr) {
        std::cerr << dll_loader::last_error_detail() << "\n";
        return std::nullopt;
    }

    auto metainfo = fn_plugin_meta();
    if (!utils::metainfo_compare(metainfo, &m_metainfos[plugin_folder])) {
        std::clog << "插件元信息 metainfo, 代码中元信息结构体与 metainfo.json 不匹配\n";
    }
    
    
    auto fn_plugin_create = dll_lib->find_function<PluginCreateFunc>(STRINGIFY(plugin_create));
    if (fn_plugin_create == nullptr) {
        std::cerr << dll_loader::last_error_detail() << "\n";
        return std::nullopt;
    }
    auto fn_plugin_destroy = dll_lib->find_function<PluginDestroyFunc>(STRINGIFY(plugin_destroy));
    if (fn_plugin_destroy == nullptr) {
        std::cerr << dll_loader::last_error_detail() << "\n";
        return std::nullopt;
    }
    auto plugin = fn_plugin_create();

    m_dlls[plugin_folder] = {
        .fn_plugin_meta = fn_plugin_meta,
        .fn_plugin_create = fn_plugin_create,
        .fn_plugin_destory = fn_plugin_destroy,
        .dll = std::move(dll_lib.value()),
        .is_init = false,
        .plugin = plugin
    };
    return &m_dlls[plugin_folder];
}

} // plugin