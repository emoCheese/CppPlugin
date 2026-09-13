#include "plugin_factory.h"
#include <dll_loader/dynamic_library.hpp>

using IPlugin = plugin::IPlugin;

void PluginFactory::scanFileFolder(const std::filesystem::path& plugin_folder)
{

}

IPlugin* PluginFactory::loadPlugin(const char* name)
{
    auto plugin = load(name);
    plugin->initialize();
    return plugin;
}

void PluginFactory::unloadPlugin(IPlugin* plugin)
{
    plugin->shutdown();
}


IPlugin* PluginFactory::load(const char* name)
{
}