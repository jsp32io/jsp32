#pragma once

#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <json/json.h>

namespace jsp32 {

struct HttpError : std::runtime_error {
    long status;
    HttpError(long status, const std::string &message) : std::runtime_error(message), status(status) {}
};

struct HttpResponse {
    long status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string header(const std::string &name) const;
};

struct FsEntry {
    std::string path;
    bool directory = false;
    uint64_t size = 0;
};

struct RegisterSnapshot {
    uint64_t revision = 0;
    std::map<std::string, uint16_t> values;
};

uint64_t parse_register_revision(const Json::Value &value);
bool register_info_ready(const Json::Value &info);
unsigned register_area_count(const Json::Value &info, char area);

std::string normalize_url(const std::string &input);
std::string url_encode(const std::string &input);

class Api {
public:
    explicit Api(std::string url);
    const std::string &url() const { return url_; }
    HttpResponse request(const std::string &method, const std::string &resource,
                         const std::string &body = {},
                         const std::vector<std::string> &headers = {},
                         long timeout_ms = 5000) const;
    Json::Value json(const std::string &method, const std::string &resource,
                     const Json::Value *body = nullptr, long timeout_ms = 5000) const;
    Json::Value info() const;
    Json::Value set_tx_power(int max_tx_power_qdbm) const;
    Json::Value register_info() const;
    RegisterSnapshot read_registers(const std::vector<std::string> &addresses) const;
    void write_registers(const std::vector<std::pair<std::string, uint16_t>> &writes) const;
    Json::Value eval(const std::string &source) const;
    std::vector<FsEntry> list(const std::string &directory) const;
    HttpResponse file(const std::string &path) const;
    void mkdir(const std::string &path) const;
    void put_file(const std::string &path, const std::string &bytes,
                  const std::string &etag = {}) const;
    void delete_file(const std::string &path, const std::string &etag = {}) const;
    void reboot() const;

private:
    std::string url_;
};

} // namespace jsp32
