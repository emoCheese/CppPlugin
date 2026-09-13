#include "plugin_a.h"
#include <cstdio>

const static plugin::PluginMetaInfo info {
        .api_version = 1, 
        .name = "plugin_a",
        .version = "1", 
        .description = "",
        .author = ""
    };

const plugin::PluginMetaInfo* plugin_get_meta_info()
{
    return &info;
}


Plugin_A::Plugin_A() 
    : m_name("plugin a")
{}

const char* Plugin_A::name() const
{
    return m_name.c_str();
}

bool Plugin_A::initialize()
{
    std::printf("%s initialized!!!", m_name.c_str());
    return true;
}


void Plugin_A::shutdown()
{
    std::printf("%s shutdown!!!", m_name.c_str());
}