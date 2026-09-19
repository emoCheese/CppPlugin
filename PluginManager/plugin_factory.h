#pragma once
#include "metainfo.h"
#include <filesystem>
#include <dll_loader/dynamic_library.hpp>
#include <unordered_map>
#include <plugin_api.h>

/// 插件必须是 插件名.so，不能有前缀lib，

namespace fs = std::filesystem;
using PluginPath = fs::path;


namespace plugin {

struct Dll {
    PluginMetaFunc     fn_plugin_meta    = nullptr;     // 获取插件元信息
    PluginCreateFunc   fn_plugin_create  = nullptr;     // 创建插件
    PluginDestroyFunc  fn_plugin_destory = nullptr;    // 销毁实例
    dll_loader::DynamicLibrary dll;
    IPlugin* plugin = nullptr;
};

class PluginFactory
{
public:
    /// 递归扫描插件目录下的 meta info
    void scanFileFolder(const fs::path& plugins_folder);

    IPlugin* loadPlugin(std::string_view name);
    void unloadPlugin(IPlugin* plugin);

private:
    /// @param plugin_folder 必须为插件目录的绝对路径（相对路径未测试）,
    //                       该路径下存放 metainfo.json 与 so 文件
    /// @param name          为 插件 metainfo.json 中注册的插件名，应当与 so 文件名相同
    IPlugin* load(const fs::path& plugin_folder, std::string_view name);

private:
    fs::path m_plugins_path;
    std::unordered_map<PluginPath, PluginMeta> m_metainfos;   // metainfo.json 中获取
    std::unordered_map<PluginPath, Dll>        m_dlls;
};
 
}
