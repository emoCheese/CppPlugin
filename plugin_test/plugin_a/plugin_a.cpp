#include "plugin_a.h"
#include <cstdio>


static const char* g_name = "plugin_a";
static const char* g_version = "1.0.0";
static const char* g_desc = "example plugin";
static const char* g_author = "author";

const static plugin::PluginMetaInfo info {
        .api_version = 1, 
        .name = g_name,
        .version = g_version, 
        .description = g_desc,
        .author = g_author
    };



const plugin::PluginMetaInfo* plugin_get_meta_info() { return &info; }
plugin::IPlugin* plugin_create() { return new Plugin_A(); }
void plugin_destroy(plugin::IPlugin* p) { delete p; }


bool Plugin_A::initialize()
{
    std::printf("%s initialized!!!\n", m_name.c_str());
    return true;
}


void Plugin_A::shutdown()
{
    std::printf("%s shutdown!!!\n", m_name.c_str());
}

void Plugin_A::do_something() {
    std::printf("do something\n");
}

Plugin_A::Plugin_A() 
    : m_name("plugin a")
{}

const char* Plugin_A::name() const
{
    return m_name.c_str();
}

