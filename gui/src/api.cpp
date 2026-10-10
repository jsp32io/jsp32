#include "api.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <curl/curl.h>
#include <memory>
#include <sstream>

namespace jsp32 {

static std::string trim(std::string value)
{
    auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

static std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

std::string HttpResponse::header(const std::string &name) const
{
    auto it = headers.find(lower(name));
    return it == headers.end() ? "" : it->second;
}

std::string normalize_url(const std::string &input)
{
    std::string text = trim(input);
    if (text.empty()) throw std::invalid_argument("请输入设备地址");
    if (text.find("://") == std::string::npos) text = "http://" + text;
    CURLU *raw = curl_url();
    if (!raw) throw std::runtime_error("无法解析设备地址");
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(raw, curl_url_cleanup);
    if (curl_url_set(raw, CURLUPART_URL, text.c_str(), 0) != CURLUE_OK) {
        throw std::invalid_argument("设备地址无效");
    }
    auto part = [raw](CURLUPart key) {
        char *value = nullptr;
        if (curl_url_get(raw, key, &value, 0) != CURLUE_OK) return std::string{};
        std::string result(value);
        curl_free(value);
        return result;
    };
    auto scheme = lower(part(CURLUPART_SCHEME));
    auto host = part(CURLUPART_HOST);
    auto path = part(CURLUPART_PATH);
    if ((scheme != "http" && scheme != "https") || host.empty() ||
        !part(CURLUPART_USER).empty() || !part(CURLUPART_PASSWORD).empty() ||
        !part(CURLUPART_QUERY).empty() || !part(CURLUPART_FRAGMENT).empty()) {
        throw std::invalid_argument("设备地址须为 HTTP(S) 主机地址");
    }
    if (path != "/" && path != "/api/v1") {
        throw std::invalid_argument("地址路径只能为空或 /api/v1");
    }
    std::string result = scheme + "://" + host;
    if (auto port = part(CURLUPART_PORT); !port.empty()) result += ":" + port;
    return result;
}

std::string url_encode(const std::string &input)
{
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += '%';
            result += hex[c >> 4];
            result += hex[c & 15];
        }
    }
    return result;
}

static size_t body_callback(char *data, size_t size, size_t count, void *context)
{
    auto *body = static_cast<std::string *>(context);
    size_t length = size * count;
    if (body->size() + length > 12 * 1024 * 1024) return 0;
    body->append(data, length);
    return length;
}

static size_t header_callback(char *data, size_t size, size_t count, void *context)
{
    auto *headers = static_cast<std::map<std::string, std::string> *>(context);
    std::string line(data, size * count);
    auto split = line.find(':');
    if (split != std::string::npos) {
        (*headers)[lower(line.substr(0, split))] = trim(line.substr(split + 1));
    }
    return size * count;
}

Api::Api(std::string url) : url_(normalize_url(url)) {}

HttpResponse Api::request(const std::string &method, const std::string &resource,
                          const std::string &body,
                          const std::vector<std::string> &headers,
                          long timeout_ms) const
{
    CURL *raw = curl_easy_init();
    if (!raw) throw std::runtime_error("无法初始化 HTTP 客户端");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw, curl_easy_cleanup);
    std::string endpoint = url_ + "/api/v1/" + resource;
    HttpResponse response;
    curl_easy_setopt(raw, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(raw, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT_MS, 1800L);
    curl_easy_setopt(raw, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(raw, CURLOPT_WRITEFUNCTION, body_callback);
    curl_easy_setopt(raw, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(raw, CURLOPT_HEADERFUNCTION, header_callback);
    curl_easy_setopt(raw, CURLOPT_HEADERDATA, &response.headers);
    curl_easy_setopt(raw, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(raw, CURLOPT_USERAGENT, "jsp32-gui/1");
    if (method == "HEAD") curl_easy_setopt(raw, CURLOPT_NOBODY, 1L);
    if (method == "POST" || method == "PUT") {
        curl_easy_setopt(raw, CURLOPT_POSTFIELDS, body.data());
        curl_easy_setopt(raw, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(body.size()));
    }
    curl_slist *list = nullptr;
    for (const auto &item : headers) list = curl_slist_append(list, item.c_str());
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned(list, curl_slist_free_all);
    if (list) curl_easy_setopt(raw, CURLOPT_HTTPHEADER, list);
    auto error = curl_easy_perform(raw);
    if (error != CURLE_OK) {
        throw std::runtime_error(std::string("设备通信失败: ") + curl_easy_strerror(error));
    }
    curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &response.status);
    if (response.status < 200 || response.status >= 300) {
        std::string message = "HTTP " + std::to_string(response.status);
        Json::Value payload;
        Json::CharReaderBuilder reader;
        std::string errors;
        std::istringstream input(response.body);
        if (Json::parseFromStream(reader, input, &payload, &errors) && payload["error"].isObject()) {
            auto detail = payload["error"].get("message", "").asString();
            if (!detail.empty()) message += ": " + detail;
        }
        throw HttpError(response.status, message);
    }
    return response;
}

Json::Value Api::json(const std::string &method, const std::string &resource,
                      const Json::Value *body, long timeout_ms) const
{
    std::string data;
    std::vector<std::string> headers{"Accept: application/json"};
    if (body) {
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "";
        data = Json::writeString(writer, *body);
        headers.push_back("Content-Type: application/json");
    }
    auto response = request(method, resource, data, headers, timeout_ms);
    Json::Value result;
    Json::CharReaderBuilder reader;
    std::string errors;
    std::istringstream input(response.body);
    if (!Json::parseFromStream(reader, input, &result, &errors)) {
        throw std::runtime_error("设备返回的 JSON 无效: " + errors);
    }
    return result;
}

Json::Value Api::info() const { return json("GET", "system/info"); }
Json::Value Api::set_tx_power(int max_tx_power_qdbm) const
{
    Json::Value body;
    body["maxTxPowerQdbm"] = max_tx_power_qdbm;
    return json("POST", "system/tx-power", &body);
}
bool register_info_ready(const Json::Value &info)
{
    if (!info.isObject() || !info["areas"].isObject()) return false;
    for (char area : {'X', 'Y', 'M', 'D'})
        if (!info["areas"][std::string(1, area)].isUInt()) return false;
    return true;
}
unsigned register_area_count(const Json::Value &info, char area)
{
    const auto &count = info["areas"][std::string(1, area)];
    return count.isUInt() ? count.asUInt() : 0;
}
Json::Value Api::register_info() const
{
    auto info = json("GET", "registers");
    if (!register_info_ready(info)) throw std::runtime_error("设备返回的寄存器区域信息不完整");
    return info;
}

uint64_t parse_register_revision(const Json::Value &value)
{
    if (!value.isString()) throw std::runtime_error("寄存器修订号无效");
    const std::string text = value.asString();
    uint64_t revision = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), revision);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        throw std::runtime_error("寄存器修订号无效");
    return revision;
}

RegisterSnapshot Api::read_registers(const std::vector<std::string> &addresses) const
{
    RegisterSnapshot result;
    if (addresses.empty()) return result;
    Json::Value body;
    body["addresses"] = Json::Value(Json::arrayValue);
    for (const auto &address : addresses) body["addresses"].append(address);
    auto reply = json("POST", "registers/read", &body);
    result.revision = parse_register_revision(reply["revision"]);
    for (const auto &item : reply["values"]) {
        auto value = item["value"];
        result.values[item["address"].asString()] = value.isBool() ? (value.asBool() ? 1 : 0)
                                                           : static_cast<uint16_t>(value.asUInt());
    }
    return result;
}

void Api::write_registers(const std::vector<std::pair<std::string, uint16_t>> &writes) const
{
    if (writes.empty()) return;
    Json::Value body;
    body["writes"] = Json::Value(Json::arrayValue);
    for (const auto &[address, value] : writes) {
        Json::Value item;
        item["address"] = address;
        item["value"] = value;
        body["writes"].append(item);
    }
    json("POST", "registers/write", &body);
}

Json::Value Api::eval(const std::string &source) const
{
    Json::Value body;
    body["source"] = source;
    body["mode"] = "expr";
    body["await"] = true;
    body["timeoutMs"] = 5000;
    auto reply = json("POST", "quickjs/eval", &body, 7000);
    if (!reply.get("ok", false).asBool()) {
        throw std::runtime_error(reply["error"].get("message", "JavaScript 执行失败").asString());
    }
    return reply["value"];
}

std::vector<FsEntry> Api::list(const std::string &directory) const
{
    std::vector<FsEntry> result;
    std::string after;
    for (;;) {
        std::string resource = "fs/files?limit=100&path=" + url_encode(directory);
        if (!after.empty()) resource += "&after=" + url_encode(after);
        auto page = json("GET", resource);
        for (const auto &file : page["files"]) {
            result.push_back({file["path"].asString(),
                              file.get("directory", false).asBool(),
                              file.get("size", 0).asUInt64()});
        }
        if (page["nextAfter"].isNull()) break;
        auto next = page["nextAfter"].asString();
        if (next.empty() || next == after) throw std::runtime_error("文件列表分页未前进");
        after = next;
    }
    return result;
}

HttpResponse Api::file(const std::string &path) const
{
    return request("GET", "fs/file?path=" + url_encode(path), {}, {}, 15000);
}

void Api::mkdir(const std::string &path) const
{
    request("POST", "fs/directory?path=" + url_encode(path));
}

void Api::put_file(const std::string &path, const std::string &bytes,
                   const std::string &etag) const
{
    std::vector<std::string> headers{"Content-Type: application/octet-stream"};
    headers.push_back(etag.empty() ? "If-None-Match: *" : "If-Match: " + etag);
    request("PUT", "fs/file?path=" + url_encode(path), bytes, headers, 30000);
}

void Api::delete_file(const std::string &path, const std::string &etag) const
{
    std::vector<std::string> headers;
    if (!etag.empty()) headers.push_back("If-Match: " + etag);
    request("DELETE", "fs/file?path=" + url_encode(path), {}, headers);
}

void Api::reboot() const { json("POST", "ota/reboot"); }

} // namespace jsp32
