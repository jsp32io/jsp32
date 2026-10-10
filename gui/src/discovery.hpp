#pragma once
#include <atomic>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace jsp32 {
struct Candidate { std::string name; std::string url; };
#ifdef _WIN32
struct WindowsDiscoveryState;
#endif
class Discovery {
public:
    Discovery();
    ~Discovery();
    std::vector<Candidate> take();
private:
    void run();
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::vector<Candidate> found_;
#ifdef _WIN32
    std::shared_ptr<WindowsDiscoveryState> windows_;
#endif
};
}
