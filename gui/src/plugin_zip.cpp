#include "plugin_zip.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <filesystem>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <memory>

namespace jsp32 {
namespace {
#ifdef _WIN32
std::filesystem::path native_path(const std::string &utf8)
{
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}
#endif

bool valid_id(const std::string &id)
{
    static const std::regex pattern("[a-z][a-z0-9_-]{0,31}");
    return std::regex_match(id, pattern) && id != "id" && id != "config" && id != "onCleanup";
}
void check_path(const std::string &path)
{
    if (path.empty() || path[0] == '/' || path.find('\\') != std::string::npos ||
        path.find("//") != std::string::npos) throw std::runtime_error("ZIP 含非法路径");
    size_t start = 0;
    while (start < path.size()) {
        auto end = path.find('/', start);
        auto part = path.substr(start, end == std::string::npos ? end : end - start);
        if (part == "." || part == "..") throw std::runtime_error("ZIP 含非法路径");
        if (end == std::string::npos) break;
        start = end + 1;
    }
}
std::string compact_json(const Json::Value &value)
{
    Json::StreamWriterBuilder writer; writer["indentation"] = "";
    return Json::writeString(writer, value);
}
Json::Value parse_json(const std::string &body)
{
    Json::Value value; Json::CharReaderBuilder reader; std::string errors;
    std::istringstream stream(body);
    if (!Json::parseFromStream(reader, stream, &value, &errors) || !value.isObject())
        throw std::runtime_error("设备上的 JSON 文件无效: " + errors);
    return value;
}
void put_existing(const Api &api, const std::string &path, const std::string &bytes)
{
    std::string etag;
    try { etag = api.file(path).header("etag"); }
    catch (const HttpError &error) { if (error.status != 404) throw; }
    api.put_file(path, bytes, etag);
}
void ensure_dir(const Api &api, const std::string &path)
{
    try { api.mkdir(path); }
    catch (const HttpError &error) { if (error.status != 409) throw; }
}
void collect(const Api &api, const std::string &directory, PluginPackage &package)
{
    for (const auto &entry : api.list(directory)) {
        if (entry.directory) collect(api, entry.path, package);
        else {
            auto prefix = "/plugins/" + package.id + "/";
            if (entry.path.rfind(prefix, 0) != 0) throw std::runtime_error("插件文件路径异常");
            package.files.push_back({entry.path.substr(prefix.size()), api.file(entry.path).body});
        }
    }
}
}
PluginPackage read_plugin_zip(const std::string &path)
{
    std::unique_ptr<archive, decltype(&archive_read_free)> ar(archive_read_new(), archive_read_free);
    archive_read_support_format_zip(ar.get()); archive_read_support_filter_all(ar.get());
#ifdef _WIN32
    if (archive_read_open_filename_w(ar.get(), native_path(path).c_str(), 10240) != ARCHIVE_OK)
#else
    if (archive_read_open_filename(ar.get(), path.c_str(), 10240) != ARCHIVE_OK)
#endif
        throw std::runtime_error(archive_error_string(ar.get()));
    PluginPackage result; std::set<std::string> paths; size_t total = 0;
    archive_entry *entry;
    int status = ARCHIVE_OK;
    while ((status = archive_read_next_header(ar.get(), &entry)) == ARCHIVE_OK) {
        std::string name = archive_entry_pathname(entry);
        check_path(name);
        auto slash = name.find('/');
        if (slash == std::string::npos) throw std::runtime_error("ZIP 必须只有一个插件根目录");
        std::string id = name.substr(0, slash);
        if (!valid_id(id)) throw std::runtime_error("无效的插件 ID: " + id);
        if (result.id.empty()) result.id = id;
        if (result.id != id) throw std::runtime_error("ZIP 包含多个插件根目录");
        if (archive_entry_filetype(entry) == AE_IFDIR) { archive_read_data_skip(ar.get()); continue; }
        if (archive_entry_filetype(entry) != AE_IFREG) throw std::runtime_error("ZIP 不允许链接或特殊文件");
        auto relative = name.substr(slash + 1);
        check_path(relative);
        if (!paths.insert(relative).second || paths.size() > 256) throw std::runtime_error("ZIP 文件重复或过多");
        std::string bytes; char buffer[8192]; la_ssize_t read;
        while ((read = archive_read_data(ar.get(), buffer, sizeof(buffer))) > 0) {
            total += static_cast<size_t>(read);
            if (total > 8 * 1024 * 1024) throw std::runtime_error("ZIP 解压后超过 8 MiB");
            bytes.append(buffer, static_cast<size_t>(read));
        }
        if (read < 0) throw std::runtime_error(archive_error_string(ar.get()));
        result.files.push_back({relative, std::move(bytes)});
    }
    if (status != ARCHIVE_EOF) throw std::runtime_error(archive_error_string(ar.get()));
    if (!paths.contains("index.js")) throw std::runtime_error("插件缺少 index.js");
    return result;
}
void write_plugin_zip(const std::string &path, const PluginPackage &package)
{
    if (!valid_id(package.id)) throw std::runtime_error("无效的插件 ID");
    std::unique_ptr<archive, decltype(&archive_write_free)> ar(archive_write_new(), archive_write_free);
    archive_write_set_format_zip(ar.get());
#ifdef _WIN32
    if (archive_write_open_filename_w(ar.get(), native_path(path).c_str()) != ARCHIVE_OK)
#else
    if (archive_write_open_filename(ar.get(), path.c_str()) != ARCHIVE_OK)
#endif
        throw std::runtime_error(archive_error_string(ar.get()));
    for (const auto &file : package.files) {
        check_path(file.path);
        std::unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
        std::string name = package.id + "/" + file.path;
        archive_entry_set_pathname(entry.get(), name.c_str());
        archive_entry_set_filetype(entry.get(), AE_IFREG);
        archive_entry_set_perm(entry.get(), 0644);
        archive_entry_set_size(entry.get(), static_cast<la_int64_t>(file.bytes.size()));
        if (archive_write_header(ar.get(), entry.get()) != ARCHIVE_OK ||
            archive_write_data(ar.get(), file.bytes.data(), file.bytes.size()) != static_cast<la_ssize_t>(file.bytes.size()))
            throw std::runtime_error(archive_error_string(ar.get()));
    }
    if (archive_write_close(ar.get()) != ARCHIVE_OK) throw std::runtime_error(archive_error_string(ar.get()));
}
void upload_plugin(const Api &api, const PluginPackage &package)
{
    ensure_dir(api, "/plugins");
    std::string root = "/plugins/" + package.id;
    ensure_dir(api, root);
    std::set<std::string> dirs;
    for (const auto &file : package.files) {
        for (size_t slash = file.path.find('/'); slash != std::string::npos;
             slash = file.path.find('/', slash + 1))
            dirs.insert(root + "/" + file.path.substr(0, slash));
    }
    for (const auto &dir : dirs) ensure_dir(api, dir);
    for (const auto &file : package.files) {
        if (file.path == "index.js" || file.path == "config.json") continue;
        put_existing(api, root + "/" + file.path, file.bytes);
    }
    // Existing device settings are kept when updating a package.
    for (const auto &file : package.files) if (file.path == "config.json") {
        try { api.file(root + "/config.json"); }
        catch (const HttpError &error) { if (error.status == 404) api.put_file(root + "/config.json", file.bytes); else throw; }
    }
    for (const auto &file : package.files) if (file.path == "index.js")
        put_existing(api, root + "/index.js", file.bytes);
}
PluginPackage download_plugin(const Api &api, const std::string &id)
{
    if (!valid_id(id)) throw std::runtime_error("无效的插件 ID");
    PluginPackage result{id, {}}; collect(api, "/plugins/" + id, result); return result;
}
void set_plugin_enabled(const Api &api, const std::string &id, bool enabled)
{
    if (!valid_id(id)) throw std::runtime_error("无效的插件 ID");
    Json::Value meta(Json::objectValue); std::string etag;
    try { auto response = api.file("/plugins/meta.json"); meta = parse_json(response.body); etag = response.header("etag"); }
    catch (const HttpError &error) { if (error.status != 404) throw; }
    if (!meta[id].isObject()) meta[id] = Json::Value(Json::objectValue);
    meta[id]["disabled"] = !enabled;
    api.put_file("/plugins/meta.json", compact_json(meta), etag);
}
void set_plugin_config(const Api &api, const std::string &id, const Json::Value &config)
{
    if (!valid_id(id) || !config.isObject()) throw std::runtime_error("插件配置须为 JSON 对象");
    put_existing(api, "/plugins/" + id + "/config.json", compact_json(config));
}
std::vector<std::string> list_plugin_ids(const Api &api)
{
    std::vector<std::string> ids;
    try { for (const auto &entry : api.list("/plugins")) {
        auto prefix = std::string("/plugins/");
        if (entry.directory && entry.path.rfind(prefix, 0) == 0) {
            auto id = entry.path.substr(prefix.size());
            if (valid_id(id) && id.find('/') == std::string::npos) ids.push_back(id);
        }
    }} catch (const HttpError &error) { if (error.status != 404) throw; }
    return ids;
}
}
