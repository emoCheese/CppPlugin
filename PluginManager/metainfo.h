#pragma once
#include <format>
#include <string>
#include <cstdint>

/// 插件宿主使用
struct PluginMeta {
    std::uint32_t       api_version = 0;
    std::string         name;
    std::string         version;
    std::string         description;
    std::string         author;

    std::string to_string() const {
        return std::format("name: {}-{}, author: {}", name, version, author, description);
    }
};