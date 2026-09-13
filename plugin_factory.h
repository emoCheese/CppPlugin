#pragma once
#include "plugin_api.h"
#include <filesystem>




class PluginFactory
{
public:
    void registry();

    void scanFileFolder(const std::filesystem::path& plugin_folder);

    void loadPlugin(const char* name);
    void unloadPlugin(IPlugin* plugin);



private:


};