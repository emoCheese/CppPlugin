#pragma once
#include <filesystem>
#include <dll_loader/dynamic_library.hpp>
#include <vector>
#include <plugin_api.h>



class PluginFactory
{
public:
    using IPlugin = plugin::IPlugin;
    using PluginMetaInfo = plugin::PluginMetaInfo;


    /// 递归扫描插件目录下的 meta info
    void scanFileFolder(const std::filesystem::path& plugin_folder);

    IPlugin* loadPlugin(const char* name);
    void unloadPlugin(IPlugin* plugin);

private:
    IPlugin* load(const char* name);

private:
    std::vector<PluginMetaInfo> m_metainfos;
    std::vector<dll_loader::DynamicLibrary> m_dlls;

};