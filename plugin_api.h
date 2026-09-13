// plugin_api.h  —— 宿主与插件共用的头文件，属于稳定 ABI
#pragma once
#include <cstdint>

#define PLUGIN_API_VERSION 1
#define PLUGIN_ENTRY_SYMBOL "plugin_get_instance"
#define PLUGIN_META_SYMBOL  "plugin_get_meta_info"

extern "C" {

struct PluginMetaInfo {
    uint32_t    api_version;   // 必须与宿主匹配
    const char* name;
    const char* version;
    const char* description;
    const char* author;
};

// 插件生命周期对象接口（纯虚，稳定 ABI）
class IPlugin {
public:
    virtual ~IPlugin() = default;
    virtual const char* name() const = 0;
    virtual bool        initialize()   = 0;
    virtual void        shutdown()     = 0;
};

// 插件导出：元信息
const PluginMetaInfo* plugin_get_meta_info();

// 插件导出：创建/销毁实例
IPlugin* plugin_create();
void     plugin_destroy(IPlugin*);

} // extern "C"