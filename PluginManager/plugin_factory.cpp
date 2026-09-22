#include "plugin_factory.h"
#include <algorithm>
#include <dll_loader/dynamic_library.hpp>
#include "dll_loader/load_error.hpp"
#include "plugin_api.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <format>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>



namespace plugin {

struct Dll  {
    PluginMetaFunc              fn_plugin_meta    = nullptr;     // 获取插件元信息
    PluginCreateFunc            fn_plugin_create  = nullptr;     // 创建插件
    PluginDestroyFunc           fn_plugin_destory = nullptr;    // 销毁实例
    dll_loader::DynamicLibrary  dll;
    bool                        is_init = false;
    IPlugin*                    plugin = nullptr;
};

namespace fs = std::filesystem;

void PluginFactory::scanFileFolder(const fs::path& plugin_folder)
{
    auto info = utils::find_metainfo(plugin_folder);
    for(const auto& [key, val] : info) 
    {
        if (m_metainfos.find(key) == m_metainfos.end()) {
            m_metainfos.insert({key, val});
        }
    }
}

void PluginFactory::unloadAll() noexcept
{
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

IPlugin* PluginFactory::loadPlugin(std::string_view name)
{
    /// 这里的 key 就是 fs::path 指向 so 文件目录的绝对路径
    for(auto& [key, value] : m_metainfos) 
    {
        if (value.name.compare(name) == 0) {
            try {
                auto dll = load(key, name);
                if (!dll.has_value()) 
                    return nullptr;
                
                auto plugin = dll.value()->plugin;
                if (dll.value()->is_init == true) {
                    std::clog << std::format("插件 {} 已经初始化\n", dll.value()->plugin->name());
                    return dll.value()->plugin;
                }
                if (!plugin->initialize()) {
                    unloadPlugin(plugin);              // 初始化失败必须按 shutdown→destroy→unload 回收
                    return nullptr;
                }
                dll.value()->is_init = true;
                return plugin;
            } catch (std::runtime_error& ec) {
                std::cerr << ec.what() << "\n";
                return nullptr;
            }
        }
    }
    return nullptr;
}

bool PluginFactory::unloadPlugin(IPlugin* plugin)
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

IPlugin* PluginFactory::getPlugin(std::string_view name)
{
    for(const auto& [key, dll] : m_dlls) {
        if (dll.plugin->name() == name) {
            return dll.plugin;
        }
    }
    return nullptr;
}

/// @todo 先查找是否存在已经打开的 插件 防止重复加载
std::optional<Dll*> PluginFactory::load(const fs::path& plugin_folder, std::string_view name)
{
    auto it = std::find_if(m_dlls.begin(), m_dlls.end(), [&plugin_folder](const auto& kv) {
        return kv.first == plugin_folder;
    });
    if (it != m_dlls.end()) {
        // 插件被加载过
        return std::nullopt;
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