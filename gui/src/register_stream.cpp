#include "register_stream.hpp"
#include "model.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <chrono>
#include <memory>
#include <sstream>

namespace jsp32 {
RegisterEventDecoder::RegisterEventDecoder(std::function<void(RegisterEvent)> emit) : emit_(std::move(emit)) {}
void RegisterEventDecoder::feed(const char *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == '\n') {
            if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
            line(pending_); pending_.clear();
        } else {
            pending_ += data[i];
            if (pending_.size() > 4096) throw std::runtime_error("寄存器 SSE 行过长");
        }
    }
}
void RegisterEventDecoder::line(const std::string &text)
{
    if (text.empty()) {
        if (event_ == "dropped") emit_({RegisterEvent::Resync});
        else if (event_ == "change") {
            Json::Value value; Json::CharReaderBuilder builder;
            std::string errors;
            std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
            if (!reader->parse(data_.data(), data_.data() + data_.size(), &value, &errors) ||
                !value.isObject() || !value["a"].isString())
                throw std::runtime_error("无效的寄存器 SSE 事件");
            auto address = parse_address(value["a"].asString());
            const auto &v = value["v"];
            if ((address.area == 'D' && (!v.isUInt() || v.asUInt() > 65535)) ||
                (address.area != 'D' && !v.isBool()))
                throw std::runtime_error("无效的寄存器 SSE 数值");
            emit_({RegisterEvent::Change, parse_register_revision(value["r"]),
                   address_text(address.area, address.index),
                   static_cast<uint16_t>(v.isBool() ? v.asBool() : v.asUInt())});
        }
        event_.clear(); data_.clear(); return;
    }
    if (text[0] == ':') return;
    auto colon = text.find(':');
    auto field = text.substr(0, colon);
    auto value = colon == std::string::npos ? std::string() : text.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') value.erase(0, 1);
    if (field == "event") event_ = value;
    else if (field == "data") {
        if (!data_.empty()) data_ += '\n';
        data_ += value;
        if (data_.size() > 8192) throw std::runtime_error("寄存器 SSE 事件过长");
    }
}
void merge_register_value(std::map<std::string, uint16_t> &values,
                          std::map<std::string, uint64_t> &revisions,
                          const std::string &address, uint16_t value, uint64_t revision)
{
    auto found = revisions.find(address);
    if (found != revisions.end() && found->second > revision) return;
    values[address] = value; revisions[address] = revision;
}
std::string register_subscription_query(const std::vector<std::string> &addresses,
                                        const std::string &start, unsigned count)
{
    std::string query;
    if (!start.empty() && count) query = "start=" + url_encode(start) + "&count=" + std::to_string(count);
    std::vector<std::string> unique = addresses;
    std::sort(unique.begin(), unique.end()); unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
    std::string list;
    for (const auto &address : unique) {
        if (!start.empty() && count) {
            auto a = parse_address(address), s = parse_address(start);
            if (a.area == s.area && a.index >= s.index && a.index - s.index < count) continue;
        }
        if (!list.empty()) list += ',';
        list += address;
    }
    if (!list.empty()) {
        if (!query.empty()) query += '&';
        query += "addresses=" + url_encode(list);
    }
    if (query.size() + sizeof("/api/v1/registers/events?") > 512)
        throw std::runtime_error("监控订阅地址过多，请减少监控项");
    return query;
}
RegisterStream::~RegisterStream() { stop(); }
void RegisterStream::stop()
{
    stopping_ = true; wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_); events_.clear();
}
void RegisterStream::emit(RegisterEvent event)
{
    std::lock_guard lock(mutex_);
    if (events_.size() >= 4096) {
        // A lost Connected event could hide a device reboot (revision resets).
        events_.clear(); events_.push_back({RegisterEvent::Connected});
        events_.push_back({RegisterEvent::Resync});
    }
    events_.push_back(std::move(event));
}
std::deque<RegisterEvent> RegisterStream::take()
{
    std::lock_guard lock(mutex_); std::deque<RegisterEvent> result; result.swap(events_); return result;
}
void RegisterStream::start(const std::string &url, const std::string &query)
{
    stop(); stopping_ = false;
    thread_ = std::thread([this, url, query] {
        unsigned delay = 1;
        while (!stopping_) {
            struct Transfer {
                RegisterStream *owner;
                CURL *curl;
                RegisterEventDecoder decoder;
                bool connected = false;
                std::string error;
                std::string error_body;
                std::chrono::steady_clock::time_point received = std::chrono::steady_clock::now();
            } transfer{this, nullptr, RegisterEventDecoder([this](RegisterEvent e) { emit(std::move(e)); })};
            std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
            transfer.curl = curl.get();
            if (!curl) { emit({RegisterEvent::Disconnected, 0, {}, 0, "无法初始化 SSE 连接"}); break; }
            auto endpoint = url + "/api/v1/registers/events?" + query;
            curl_easy_setopt(curl.get(), CURLOPT_URL, endpoint.c_str());
            curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 1800L);
            curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
            curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, +[](void *ctx, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                auto &t = *static_cast<Transfer *>(ctx);
                return t.owner->stopping_ || std::chrono::steady_clock::now() - t.received > std::chrono::seconds(40);
            });
            curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
            curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, +[](char *data, size_t size, size_t count, void *ctx) -> size_t {
                auto &t = *static_cast<Transfer *>(ctx);
                try {
                    long status = 0; curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &status);
                    if (status != 200) {
                        t.error = "SSE HTTP " + std::to_string(status);
                        if (t.error_body.size() + size * count > 8192) return 0;
                        t.error_body.append(data, size * count);
                        return size * count;
                    }
                    if (!t.connected) {
                        char *content_type = nullptr;
                        curl_easy_getinfo(t.curl, CURLINFO_CONTENT_TYPE, &content_type);
                        if (!content_type || std::string(content_type).find("text/event-stream") != 0) {
                            t.error = "设备未返回 SSE 数据流"; return 0;
                        }
                        t.connected = true; t.owner->emit({RegisterEvent::Connected});
                    }
                    t.received = std::chrono::steady_clock::now();
                    t.decoder.feed(data, size * count); return size * count;
                } catch (const std::exception &e) { t.error = e.what(); return 0; }
            });
            auto result = curl_easy_perform(curl.get());
            if (stopping_) break;
            long status = 0; curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
            auto message = transfer.error.empty() ? (status && status != 200 ? "SSE HTTP " + std::to_string(status) :
                result == CURLE_OK ? "订阅连接已断开" : curl_easy_strerror(result)) : transfer.error;
            if (!transfer.error_body.empty()) {
                Json::Value payload; Json::CharReaderBuilder reader; std::string errors;
                std::istringstream input(transfer.error_body);
                if (Json::parseFromStream(reader, input, &payload, &errors) && payload["error"].isObject()) {
                    auto detail = payload["error"].get("message", "").asString();
                    if (!detail.empty()) message += ": " + detail;
                }
            }
            emit({RegisterEvent::Disconnected, 0, {}, 0, message});
            if (transfer.connected) delay = 1;
            std::unique_lock lock(mutex_);
            wake_.wait_for(lock, std::chrono::seconds(delay), [this] { return stopping_.load(); });
            delay = std::min(8u, delay * 2);
        }
    });
}
}
