#pragma once
#include "metainfo.h"
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <plugin_api.h>




/// 插件必须是 插件名.so，不能有前缀lib，
using PluginPath = std::filesystem::path;


namespace plugin {

struct Dll;

class PluginFactory
{
public:
    ~PluginFactory() { unloadAll(); }

    /// 递归扫描插件目录下的 meta info
    void scanFileFolder(const std::filesystem::path& plugins_folder);

    IPlugin* loadPlugin(std::string_view name);
    bool unloadPlugin(IPlugin* plugin);

    IPlugin* getPlugin(std::string_view name);  // 获取已经加载过的 plugin

private:
    /// @todo 考虑按依赖逆序销毁
    void unloadAll() noexcept;

    /// @param plugin_folder 必须为插件目录的绝对路径（相对路径未测试）,
    //                       该路径下存放 metainfo.json 与 so 文件
    /// @param name          为 插件 metainfo.json 中注册的插件名，应当与 so 文件名相同
    std::optional<Dll*> load(const std::filesystem::path& plugin_folder, std::string_view name);

private:
    std::filesystem::path m_plugins_path;

    /// 这里的 PluginPath 都必须是指向 so 文件所在文件夹的路径
    std::unordered_map<PluginPath, PluginMeta> m_metainfos;   // metainfo.json 中获取
    std::unordered_map<PluginPath, Dll>        m_dlls;
};
 
}
