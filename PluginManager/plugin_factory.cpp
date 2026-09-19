#include "plugin_factory.h"
#include <dll_loader/dynamic_library.hpp>
#include "dll_loader/load_error.hpp"
#include "plugin_api.h"
#include "utils.h"
#include <filesystem>
#include <iostream>
#include <ranges>
#include <format>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
namespace plugin {

namespace fs = std::filesystem;

void PluginFactory::scanFileFolder(const fs::path& plugin_folder)
{
    m_metainfos = utils::find_metainfo(plugin_folder);
}



IPlugin* PluginFactory::loadPlugin(std::string_view name)
{
    /// 这里的 key 就是 fs::path 指向 so 文件目录的绝对路径
    for(auto [key, value] : m_metainfos) 
    {
        if (value.name.compare(name)) {
            try {
                auto plugin = load(key, name);
                plugin->initialize();
                return plugin;
            } catch (std::runtime_error& ec) {
                std::cerr << ec.what();
                return nullptr;
            }
        }
    }
    return nullptr;
}

void PluginFactory::unloadPlugin(IPlugin* plugin)
{
    if (plugin == nullptr) return;
    /// 回收资源
    plugin->shutdown();
    for( auto& [path , dll] : m_dlls) {
        if (dll.plugin == plugin) {
            /// 销毁实例
            dll.fn_plugin_destory(plugin);
            /// 卸载 .so
            dll.dll.unload();
            break;
        }
    }

}


IPlugin* PluginFactory::load(const fs::path& plugin_folder, std::string_view name)
{
    std::string fullpath = std::format("{}/{}.so", plugin_folder.generic_string(), name);

    
    std::error_code ec;
    auto dll_lib = dll_loader::DynamicLibrary::open(fullpath, dll_loader::LoadFlags::none, ec);
    if (!dll_lib.has_value()) {
        std::cerr << dll_loader::describe_error(ec);
        dll_lib.reset();
        std::string fullpath2 = std::format("{}/lib{}.so", plugin_folder.generic_string(), name);
        dll_lib = dll_loader::DynamicLibrary::open(fullpath2, dll_loader::LoadFlags::none, ec);

        if (!dll_lib.has_value()) {
            std::cerr << dll_loader::describe_error(ec);
            return nullptr;
        }
    }
    
        
    /// 找到 plugin_create 并调用
    auto fn_plugin_meta = dll_lib->find_function<PluginMetaFunc>(STRINGIFY(plugin_get_meta_info));
    if (fn_plugin_meta == nullptr) {
        std::cerr << dll_loader::last_error_detail();
        return nullptr;
    }

    auto metainfo = fn_plugin_meta();
    if (!utils::metainfo_compare(metainfo, &m_metainfos[plugin_folder])) {
        std::clog << "插件元信息 metainfo, 代码中元信息结构体与 metainfo.json 不匹配";
    }
    
    
    auto fn_plugin_create = dll_lib->find_function<PluginCreateFunc>(STRINGIFY(plugin_create));
    if (fn_plugin_create == nullptr) {
        std::cerr << dll_loader::last_error_detail();
        return nullptr;
    }
    auto fn_plugin_destroy = dll_lib->find_function<PluginDestroyFunc>(STRINGIFY(plugin_destroy));
    if (fn_plugin_destroy == nullptr) {
        std::cerr << dll_loader::last_error_detail();
        return nullptr;
    }
    auto plugin = fn_plugin_create();
    m_dlls[plugin_folder] = {
        .fn_plugin_meta = fn_plugin_meta,
        .fn_plugin_create = fn_plugin_create,
        .fn_plugin_destory = fn_plugin_destroy,
        .dll = std::move(dll_lib.value()),
        .plugin = plugin
    };
    return plugin;
}

} // plugin