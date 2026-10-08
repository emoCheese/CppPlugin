#include <filesystem>
#include <plugin_factory.h>
#include <iostream>
// #include "plugin_a.h"

class Plugin_A;

int main(int argc, char* argv[])
{
    namespace fs = std::filesystem;
    plugin::PluginFactory plugin_factory = plugin::PluginFactory();

    plugin_factory.scanFileFolder(fs::path("../plugin_test/"));

    auto* plugin = plugin_factory.loadPlugin("plugin_a");
    if (!plugin) {
        std::cout << "plugin is nullptr\n";
        return 1;
    }
        
    std::cout << "Plugin name: " << plugin->name() << "\n";

    auto a = reinterpret_cast<Plugin_A*>(plugin);

    // a->do_something();

    return 0;
}