#pragma once
#include "api.hpp"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace jsp32 {
struct RegisterEvent {
    enum Kind { Connected, Change, Resync, Disconnected } kind;
    uint64_t revision = 0;
    std::string address;
    uint16_t value = 0;
    std::string message;
};

// Incremental SSE decoder, independent of transport and UI.
class RegisterEventDecoder {
public:
    explicit RegisterEventDecoder(std::function<void(RegisterEvent)> emit);
    void feed(const char *data, size_t size);
private:
    void line(const std::string &line);
    std::function<void(RegisterEvent)> emit_;
    std::string pending_, event_, data_;
};

class RegisterStream {
public:
    ~RegisterStream();
    void start(const std::string &url, const std::string &query);
    void stop();
    std::deque<RegisterEvent> take();
private:
    void emit(RegisterEvent event);
    std::atomic<bool> stopping_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<RegisterEvent> events_;
};

// Empty addresses must not accidentally request an unfiltered stream.
std::string register_subscription_query(const std::vector<std::string> &addresses,
                                        const std::string &start = {}, unsigned count = 0);
void merge_register_value(std::map<std::string, uint16_t> &values,
                          std::map<std::string, uint64_t> &revisions,
                          const std::string &address, uint16_t value, uint64_t revision);
}
