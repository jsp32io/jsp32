#include "app.hpp"
#include "api.hpp"
#include <algorithm>

namespace jsp32 {
void App::update_register_subscription()
{
    std::string query;
    std::vector<std::string> addresses;
    if (page == "registers" && !selected.empty() && register_info_ready(register_info)) {
        unsigned capacity = register_area_count(register_info, browse_area);
        // Clamp the page if a device now exposes a smaller register area.
        if (browse_start >= capacity) browse_start = capacity ? ((capacity - 1) / 32) * 32 : 0;
        unsigned count = capacity ? std::min(32u, capacity - browse_start) : 0;
        std::string start = count ? address_text(browse_area, browse_start) : std::string();
        for (unsigned i = 0; i < count; ++i) addresses.push_back(address_text(browse_area, browse_start + i));
        try { query = register_subscription_query({}, start, count); }
        catch (const std::exception &e) { register_status = e.what(); }
    }
    std::sort(addresses.begin(), addresses.end());
    addresses.erase(std::unique(addresses.begin(), addresses.end()), addresses.end());
    std::string key = query.empty() ? std::string() : current_url() + "?" + query;
    if (key.empty() && addresses.empty() && page == "registers" && register_info_ready(register_info))
        register_status.clear();
    if (key != subscription_key_) {
        register_stream_.stop(); subscription_key_ = key;
        ++register_generation_; register_revisions_.clear(); values.clear();
        register_stream_connected_ = false;
        register_snapshot_pending_ = !key.empty(); register_retry_at_ = 0;
        register_addresses_ = std::move(addresses);
        if (!key.empty()) {
            register_status = "正在连接寄存器订阅";
            register_stream_.start(current_url(), query);
        } else if (register_addresses_.empty()) register_status.clear();
    }
    for (auto &event : register_stream_.take()) {
        if (event.kind == RegisterEvent::Connected) {
            register_stream_connected_ = true;
            clear_error();
            ++register_generation_; register_revisions_.clear(); values.clear();
            register_snapshot_pending_ = true; register_retry_at_ = 0;
            register_status = "正在同步寄存器";
        } else if (event.kind == RegisterEvent::Change) {
            if (std::binary_search(register_addresses_.begin(), register_addresses_.end(), event.address))
                merge_register_value(values, register_revisions_, event.address, event.value, event.revision);
        } else if (event.kind == RegisterEvent::Resync) {
            register_snapshot_pending_ = true; register_retry_at_ = 0;
            register_status = "正在重新同步寄存器";
        } else {
            register_stream_connected_ = false;
            register_status = "订阅重连中 · " + event.message;
        }
    }
}
void App::refresh_registers()
{
    auto url = current_url(); if (url.empty()) return;
    auto generation = register_generation_;
    auto device = selected;
    auto addresses = register_addresses_;
    auto info = register_info;
    register_snapshot_pending_ = false;
    run([this, url, generation, device, addresses, info]() mutable {
        try {
            Api api(url);
            if (!register_info_ready(info)) info = api.register_info();
            std::vector<RegisterSnapshot> snapshots;
            unsigned max = std::max(1u, info.get("maxTransaction", 64).asUInt());
            for (size_t at = 0; at < addresses.size(); at += max) {
                auto end = std::min(addresses.size(), at + max);
                snapshots.push_back(api.read_registers({addresses.begin() + at, addresses.begin() + end}));
            }
            return std::function<void()>([this, generation, device, info, snapshots = std::move(snapshots)] {
                if (selected != device || generation != register_generation_ || page != "registers") return;
                clear_error();
                register_info = info;
                for (const auto &snapshot : snapshots)
                    for (const auto &[address, value] : snapshot.values)
                        merge_register_value(values, register_revisions_, address, value, snapshot.revision);
                if (register_stream_connected_ && !register_snapshot_pending_) register_status = "寄存器实时更新";
            });
        } catch (const std::exception &e) {
            return std::function<void()>([this, generation, device, message = std::string(e.what())] {
                if (selected != device || generation != register_generation_) return;
                register_snapshot_pending_ = true; register_retry_at_ = SDL_GetTicks() + 2000;
                register_status = "寄存器同步失败 · " + message;
            });
        }
    }, "正在读取寄存器");
}
}
