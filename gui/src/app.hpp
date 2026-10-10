#pragma once
#include "model.hpp"
#include "discovery.hpp"
#include "register_stream.hpp"
#include "plugin_config.hpp"
#include <functional>
#include <deque>
#include <set>
#include <future>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <json/json.h>
#include <SDL3/SDL.h>

namespace jsp32 {
class App {
public:
    explicit App(SDL_Window *window);
    void tick();
    void draw();
    void run(std::function<std::function<void()>()> operation, const std::string &label);
    void error(const std::string &message);
    void success(const std::string &message);
    void clear_error();
    void save();
    void select(const Device &device);
    void connect(const std::string &url, const std::string &suggested_name);
    void remove_device(const std::string &id);
    void refresh_overview();
    void request_plugin_refresh() { plugins_refresh_pending_ = true; }
    void set_tx_power();
    std::string current_url() const;
    bool busy() const { return job_.valid(); }
    void open_file(bool save, const std::string &suggested);
    void file_chosen(const std::string &path);
    Settings settings;
    std::vector<Device> discovered;
    std::string selected;
    std::string page = "overview";
    Json::Value overview_info;
    int tx_power_qdbm = 52;
    bool tx_power_dirty = false;
    std::string register_status;
    std::string notice;
    std::string job_label;
    Json::Value register_info;
    std::map<std::string,uint16_t> values;
    std::vector<std::string> plugin_ids;
    Json::Value plugin_runtime;
    std::string plugin_selected;
    PluginConfigEditor plugin_editor;
    std::string device_input;
    char browse_area = 'D';
    unsigned browse_start = 0;
    bool register_scroll_reset = true;
    std::string write_value;
    std::string write_address;
    SDL_Window *window;
private:
    void refresh_registers();
    void update_register_subscription();
    void refresh_plugins();
    void poll_device_status();
    std::string settings_path_;
    Discovery discovery_;
    std::future<std::function<void()>> job_;
    RegisterStream register_stream_;
    std::string subscription_key_;
    std::vector<std::string> register_addresses_;
    std::map<std::string, uint64_t> register_revisions_;
    uint64_t register_generation_ = 0;
    bool register_stream_connected_ = false;
    bool register_snapshot_pending_ = true;
    uint64_t register_retry_at_ = 0;
    bool plugins_refresh_pending_ = true;
    uint64_t last_overview_poll_ = 0;
    std::mutex file_mutex_;
    std::string chosen_file_;
    std::deque<Candidate> pending_candidates_;
    std::set<std::string> seen_candidate_urls_;
    std::map<std::string, uint64_t> last_device_probe_;
    bool save_dialog_ = false;
    std::string last_error_notice_;
    uint64_t notice_expires_at_ = 0;
};
void draw_devices(App &app);
void draw_overview(App &app);
void draw_registers(App &app);
void draw_plugins(App &app);
}
