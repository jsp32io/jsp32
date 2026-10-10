#pragma once
#include "api.hpp"
#include <string>
#include <vector>
namespace jsp32 {
struct PackageFile { std::string path; std::string bytes; };
struct PluginPackage { std::string id; std::vector<PackageFile> files; };
PluginPackage read_plugin_zip(const std::string &path);
void write_plugin_zip(const std::string &path, const PluginPackage &package);
void upload_plugin(const Api &api, const PluginPackage &package);
PluginPackage download_plugin(const Api &api, const std::string &id);
void set_plugin_enabled(const Api &api, const std::string &id, bool enabled);
void set_plugin_config(const Api &api, const std::string &id, const Json::Value &config);
std::vector<std::string> list_plugin_ids(const Api &api);
}
