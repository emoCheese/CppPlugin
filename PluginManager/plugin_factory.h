#pragma once
#include <filesystem>
#include <memory>
#include <plugin_api.h>


/// 插件必须是 插件名.so，不能有前缀lib，
using PluginPath = std::filesystem::path;


namespace plugin {

class PluginFactory
{
public:
    explicit PluginFactory();
    ~PluginFactory();

    /// 递归扫描插件目录下的 meta info
    void scanFileFolder(const std::filesystem::path& plugins_folder);

    IPlugin* loadPlugin(std::string_view name);
    bool unloadPlugin(IPlugin* plugin);

    IPlugin* getPlugin(std::string_view name);  // 获取已经加载过的 plugin

private:
    struct impl;
    std::unique_ptr<impl> impl_ptr;
};
 
}
