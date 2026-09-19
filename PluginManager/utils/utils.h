#pragma once
#include "plugin_api.h"
#include <filesystem>
#include <json.hpp>
#include "metainfo.h"
#include <filesystem>


namespace utils {
using PluginMetaInfo = plugin::PluginMetaInfo;

std::optional<PluginMeta> Parse(const std::filesystem::path& path);

bool metainfo_compare(const PluginMetaInfo*, const PluginMeta*);

namespace fs = std::filesystem;

std::unordered_map<fs::path, PluginMeta> find_metainfo(const fs::path &plugin_folder, int max_depth = 2);
}

