// plugin_api.h  —— 宿主与插件共用的头文件，公共头文件，属于稳定 ABI
#pragma once
#include <cstdint>

// 根据平台定义导出宏
#if defined(_WIN32) || defined(__CYGWIN__)
    #ifdef PLUGIN_BUILD_SHARED
        // 构建动态库时导出
        #define PLUGIN_API __declspec(dllexport)
    #else
        // 使用动态库时导入
        #define PLUGIN_API __declspec(dllimport)
    #endif
#else
    // GCC/Clang 等
    #ifdef PLUGIN_BUILD_SHARED
        #define PLUGIN_API __attribute__((visibility("default")))
    #else
        #define PLUGIN_API
    #endif
#endif


#define STRINGIFY_IMPL(x) #x
#define STRINGIFY(x) STRINGIFY_IMPL(x)

namespace plugin  {
    
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
    virtual const char* name() const   = 0;
    virtual bool        initialize()   = 0;     // 初始化资源
    virtual void        shutdown()     = 0;     // 清理资源
};

using PluginMetaFunc = const PluginMetaInfo* (*)();
using PluginCreateFunc = IPlugin* (*)();
using PluginDestroyFunc = void (*)(IPlugin*);

}

extern "C" {
// 插件导出：元信息
PLUGIN_API const plugin::PluginMetaInfo* plugin_get_meta_info();

// 插件导出：创建/销毁实例
PLUGIN_API plugin::IPlugin* plugin_create();
PLUGIN_API void             plugin_destroy(plugin::IPlugin*);

} // extern "C"