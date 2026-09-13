#pragma once
#include <plugin_api.h>
#include <string>

class Plugin_A : public plugin::IPlugin
{
public:
    Plugin_A();
    ~Plugin_A() = default;
    const char* name() const   override;
    bool        initialize()   override;     // 初始化资源
    void        shutdown()     override;     // 清理资源

private:
    std::string m_name;
};