#include "utils.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>

namespace utils {

std::optional<PluginMeta> Parse(const std::filesystem::path& path)
{
    using json = nlohmann::json;
    std::ifstream ifs(path.generic_string());
    if (!ifs.is_open()) {
        std::cerr << "无法打开文件: " << path << "\n";
        return std::nullopt;
    }

    json j;
    try {
        j = json::parse(ifs);
        return PluginMeta {
            .api_version = j.at("api_version"),
            .name = j.value("name", ""),
            .version = j.value("version", ""),
            .description = j.value("description", ""),
            .author = j.value("author", "")
        };
    } catch (const json::parse_error& e) {
        std::cerr << "JSON 解析失败: " << e.what() << "\n";
        return std::nullopt;
    } catch (const json::exception& e) {
        std::cerr << "JSON 异常: " << e.what() << "\n";
        return std::nullopt;
    }
};

bool metainfo_compare(const PluginMetaInfo* code_info, const PluginMeta* file_info)
{
    if (
        code_info->api_version == file_info->api_version && 
        code_info->version == file_info->version &&
        code_info->name == file_info->name
    ) {
        return true;
    }
    return false;
}

std::unordered_map<fs::path, PluginMeta> find_metainfo(const fs::path &plugin_folder, int max_depth) 
{
    std::error_code ec;

    std::unordered_map<fs::path, PluginMeta> results;

    if (max_depth <= 0 || !fs::is_directory(plugin_folder, ec)) {
        return results;
    }

    const fs::recursive_directory_iterator end;
    fs::recursive_directory_iterator it(
        plugin_folder, 
        fs::directory_options::skip_permission_denied, 
        ec
    );

    for (; it != end; it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue; // 单个条目出错不终止整体遍历
        }

        const fs::directory_entry &entry = *it;

        // 命中目标文件
        if (entry.is_regular_file(ec) && entry.path().filename() == "metainfo.json") 
        {
            try {
                auto val = Parse(entry.path());
                if (val.has_value())
                    results[fs::absolute(entry.path().parent_path())] = val.value();
            } catch (...) {
            
            }
        }

        // 已到允许的最大层级，禁止继续下钻
        if (it.depth() + 1 >= max_depth && entry.is_directory(ec)) {
            it.disable_recursion_pending();
        }
    }

    return results;
}
} // namespace utils