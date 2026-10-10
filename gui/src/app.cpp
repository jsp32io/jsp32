#include "app.hpp"
#include "font.hpp"
#include "ui_layout.hpp"
#include "api.hpp"
#include "plugin_zip.hpp"
#include <imgui.h>
#include <imgui_stdlib.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <filesystem>
#include <sstream>

namespace jsp32 {
namespace {
void dialog_callback(void *context, const char *const *files, int)
{
    if (files && files[0]) static_cast<App *>(context)->file_chosen(files[0]);
}
}
App::App(SDL_Window *win) : window(win)
{
    char *pref = SDL_GetPrefPath("JSP32", "JSP32 Control");
    if (!pref) throw std::runtime_error(SDL_GetError());
    settings_path_ = pref;
    if (!settings_path_.empty() && settings_path_.back() != '/' && settings_path_.back() != '\\')
        settings_path_ += '/';
    settings_path_ += "settings.json";
    SDL_free(pref);
    try { settings = load_settings(settings_path_); } catch (const std::exception &e) { notice = e.what(); }
    selected = settings.selected_device;
    if (std::none_of(settings.devices.begin(), settings.devices.end(),
                     [&](const Device &device) { return device.id == selected; })) selected.clear();
    if (selected.empty()) page = "devices";
}
void App::save()
{
    settings.selected_device = selected;
    try { save_settings(settings_path_, settings); } catch (const std::exception &e) { error(e.what()); }
}
void App::error(const std::string &message)
{
    notice = message;
    last_error_notice_ = message;
    notice_expires_at_ = 0;
}
void App::success(const std::string &message)
{
    clear_error();
    notice = message;
    notice_expires_at_ = SDL_GetTicks() + 3000;
}
void App::clear_error()
{
    if (!last_error_notice_.empty() && notice == last_error_notice_) notice.clear();
    last_error_notice_.clear();
}
void App::run(std::function<std::function<void()>()> operation, const std::string &label)
{
    if (busy()) return;
    job_label = label;
    job_ = std::async(std::launch::async, [operation = std::move(operation)] {
        return operation();
    });
}
std::string App::current_url() const
{
    for (const auto &device : settings.devices) if (device.id == selected) return device.url;
    return {};
}
void App::select(const Device &device)
{
    selected = device.id; page = "overview"; browse_area = 'D'; browse_start = 0; values.clear();
    register_stream_.stop(); subscription_key_.clear();
    register_stream_connected_ = false; register_scroll_reset = true;
    ++register_generation_; register_revisions_.clear();
    register_snapshot_pending_ = true; register_retry_at_ = 0;
    register_info = Json::Value(); plugin_ids.clear(); plugin_selected.clear();
    overview_info = Json::Value(); last_overview_poll_ = 0;
    tx_power_qdbm = 52; tx_power_dirty = false;
    save(); plugins_refresh_pending_ = true;
}
void App::remove_device(const std::string &id)
{
    for (const auto &device : settings.devices)
        if (device.id == id) seen_candidate_urls_.erase(device.url);
    std::erase_if(settings.devices, [&](const Device &device) { return device.id == id; });
    settings.monitors.erase(id);
    last_device_probe_.erase(id);
    if (selected == id) {
        selected.clear();
        register_stream_.stop();
        subscription_key_.clear();
    }
    save();
}
void App::connect(const std::string &url, const std::string &suggested_name)
{
    run([this, url, suggested_name] {
        Api api(url); auto info = api.info();
        if (info["firmware"].get("project", "").asString() != "jsp32")
            throw std::runtime_error("该地址不是 JSP32 设备");
        std::string id = info["mac"].get("station", "").asString();
        if (id.empty()) throw std::runtime_error("设备没有站点 MAC 地址");
        std::string name = suggested_name.empty() ? "JSP32 " + id : suggested_name;
        Device device{id, name, api.url(), info["firmware"].get("version", "").asString()};
        return std::function<void()>([this, device] {
            auto it = std::find_if(settings.devices.begin(), settings.devices.end(),
                                   [&](const Device &item) { return item.id == device.id; });
            if (it == settings.devices.end()) settings.devices.push_back(device);
            else {
                it->url = device.url;
                it->version = device.version;
                if (it->name.empty()) it->name = device.name;
            }
            auto found = std::find_if(discovered.begin(), discovered.end(),
                                      [&](const Device &item) { return item.id == device.id; });
            if (found == discovered.end()) discovered.push_back(device);
            else *found = device;
            last_device_probe_[device.id] = SDL_GetTicks();
            select(device);
            clear_error();
        });
    }, "正在连接设备");
}
void App::poll_device_status()
{
    if (busy() || page != "devices") return;
    const uint64_t now = SDL_GetTicks();
    std::vector<Device> devices = discovered;
    for (const auto &saved : settings.devices) {
        if (std::none_of(devices.begin(), devices.end(),
                         [&](const Device &item) { return item.id == saved.id; }))
            devices.push_back(saved);
    }
    for (const auto &device : devices) {
        auto checked = last_device_probe_.find(device.id);
        if (checked != last_device_probe_.end() && now - checked->second < 5000) continue;
        last_device_probe_[device.id] = now;
        run([this, device] {
            bool online = false;
            std::string version;
            try {
                const auto info = Api(device.url).json("GET", "system/info", nullptr, 1500);
                online = info["firmware"].get("project", "").asString() == "jsp32" &&
                         info["mac"].get("station", "").asString() == device.id;
                if (online) version = info["firmware"].get("version", "").asString();
            } catch (const std::exception &) {}
            return std::function<void()>([this, device, online, version] {
                const auto matches = [&](const Device &item) {
                    return item.id == device.id && item.url == device.url;
                };
                const auto current = std::find_if(discovered.begin(), discovered.end(),
                    [&](const Device &item) { return item.id == device.id; });
                if (current != discovered.end() && current->url != device.url) return;
                const bool found = std::any_of(discovered.begin(), discovered.end(), matches);
                const bool saved = std::any_of(settings.devices.begin(), settings.devices.end(), matches);
                if (!found && !saved) return;
                if (online) {
                    Device updated = device;
                    updated.version = version;
                    if (!found) discovered.push_back(updated);
                    else current->version = version;
                    bool changed = false;
                    for (auto &item : settings.devices) {
                        if (item.id == device.id && item.version != version) {
                            item.version = version;
                            changed = true;
                        }
                    }
                    if (changed) save();
                } else if (found) {
                    std::erase_if(discovered, matches);
                    seen_candidate_urls_.erase(device.url);
                }
            });
        }, "正在检查设备状态");
        return;
    }
}
void App::refresh_plugins()
{
    auto url = current_url(), device = selected;
    if (url.empty() || busy()) return;
    plugins_refresh_pending_ = false;
    run([this, url, device] {
        Api api(url); auto ids = list_plugin_ids(api);
        Json::Value runtime;
        try { runtime = api.eval("(await import('plugins')).plugins.list()"); }
        catch (const std::exception &) { runtime = Json::Value(Json::arrayValue); }
        return std::function<void()>([this, url, device, ids, runtime] {
            if (selected != device || current_url() != url) return;
            plugin_ids = ids; plugin_runtime = runtime; clear_error();
        });
    }, "正在读取插件");
}
void App::refresh_overview()
{
    auto url = current_url(), device = selected;
    if (url.empty() || busy()) return;
    last_overview_poll_ = SDL_GetTicks();
    run([this, url, device] {
        auto info = Api(url).info();
        return std::function<void()>([this, device, info] {
            if (selected != device) return;
            overview_info = info;
            const auto &power = info["network"]["wifiMaxTxPowerQdbm"];
            if (!tx_power_dirty && power.isInt()) tx_power_qdbm = power.asInt();
            clear_error();
        });
    }, "正在读取设备信息");
}
void App::set_tx_power()
{
    auto url = current_url(), device = selected;
    if (url.empty() || busy() || tx_power_qdbm < 8 || tx_power_qdbm > 84) return;
    const int requested = tx_power_qdbm;
    run([this, url, device, requested] {
        Api api(url);
        api.set_tx_power(requested);
        auto info = api.info();
        return std::function<void()>([this, device, info] {
            if (selected != device) return;
            overview_info = info;
            const auto &power = info["network"]["wifiMaxTxPowerQdbm"];
            if (power.isInt()) tx_power_qdbm = power.asInt();
            tx_power_dirty = false;
            last_overview_poll_ = SDL_GetTicks();
            success("发射功率已保存并生效");
        });
    }, "正在修改发射功率");
}
void App::tick()
{
    update_register_subscription();
    if (notice_expires_at_ && SDL_GetTicks() >= notice_expires_at_) {
        notice.clear();
        notice_expires_at_ = 0;
    }
    if (job_.valid() && job_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        job_label.clear();
        try { auto completion = job_.get(); completion(); }
        catch (const std::exception &e) { error(e.what()); }
    }
    for (const auto &candidate : discovery_.take()) {
        try {
            auto url = normalize_url(candidate.url);
            if (seen_candidate_urls_.insert(url).second) pending_candidates_.push_back({candidate.name, url});
        } catch (const std::exception &) {}
    }
    if (!busy() && !pending_candidates_.empty()) {
        auto candidate = pending_candidates_.front(); pending_candidates_.pop_front();
        run([this, candidate] {
            try {
                Api api(candidate.url); auto info = api.info();
                if (info["firmware"].get("project", "").asString() != "jsp32")
                    return std::function<void()>([] {});
                auto id = info["mac"].get("station", "").asString();
                if (id.empty()) return std::function<void()>([] {});
                Device device{id, candidate.name, candidate.url,
                              info["firmware"].get("version", "").asString()};
                return std::function<void()>([this, device] {
                    auto it = std::find_if(discovered.begin(), discovered.end(),
                        [&](const Device &existing) { return existing.id == device.id; });
                    if (it == discovered.end()) discovered.push_back(device);
                    else *it = device;
                    last_device_probe_[device.id] = SDL_GetTicks();
                    bool changed = false;
                    for (auto &saved : settings.devices) {
                        if (saved.id == device.id && saved.version != device.version) {
                            saved.version = device.version;
                            changed = true;
                        }
                    }
                    if (changed) save();
                });
            } catch (const std::exception &) { return std::function<void()>([] {}); }
        }, "正在核验发现的设备");
    }
    std::string path;
    if (!busy()) { std::lock_guard lock(file_mutex_); path.swap(chosen_file_); }
    if (!path.empty()) {
        if (save_dialog_) {
            auto url = current_url(), id = plugin_selected;
            run([this, url, id, path] {
                auto package = download_plugin(Api(url), id);
                write_plugin_zip(path, package);
                return std::function<void()>([this] { success("插件 ZIP 已下载"); });
            }, "正在下载插件");
        } else {
            auto url = current_url();
            run([this, url, path] {
                auto package = read_plugin_zip(path);
                upload_plugin(Api(url), package);
                return std::function<void()>([this] {
                    success("插件已上传，重启设备后生效"); refresh_plugins();
                });
            }, "正在上传插件");
        }
    }
    uint64_t now = SDL_GetTicks();
    if (!busy()) poll_device_status();
    if (!busy() && page == "overview" && !selected.empty() &&
        (last_overview_poll_ == 0 || now - last_overview_poll_ > 5000))
        refresh_overview();
    if (!busy() && page == "registers" && !selected.empty() &&
        (!register_info_ready(register_info) || register_snapshot_pending_) && now >= register_retry_at_)
        refresh_registers();
    if (!busy() && !selected.empty() && page == "plugins" && plugins_refresh_pending_)
        refresh_plugins();
}
void App::open_file(bool save, const std::string &suggested)
{
    save_dialog_ = save;
    static const SDL_DialogFileFilter filters[] = {{"ZIP 插件包", "zip"}};
    if (save) SDL_ShowSaveFileDialog(dialog_callback, this, window, filters, 1, suggested.c_str());
    else SDL_ShowOpenFileDialog(dialog_callback, this, window, filters, 1, nullptr, false);
}
void App::file_chosen(const std::string &path)
{
    std::lock_guard lock(file_mutex_); chosen_file_ = path;
}
void App::draw()
{
    auto &io = ImGui::GetIO();
    ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("JSP32", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();
    ImGui::PushFont(ui_heading_font(), 20.0f);
    ImGui::TextColored({0.90f,0.40f,0.18f,1}, "JSP32");
    ImGui::PopFont();
    if (!selected.empty() && page != "devices") {
        const float device_width = std::min(260.0f, ImGui::GetContentRegionAvail().x);
        align_actions_right(device_width);
        std::string name = selected;
        for (const auto &device : settings.devices) if (device.id == selected) name = device.name;
        ImGui::SetNextItemWidth(device_width);
        ImGui::SetNextWindowSizeConstraints({device_width, 0}, {device_width, FLT_MAX});
        if (ImGui::BeginCombo("##device", name.c_str())) {
            for (const auto &device : settings.devices)
                if (ImGui::Selectable((device.name + "##" + device.id).c_str(), device.id == selected)) select(device);
            ImGui::Separator();
            if (ImGui::Selectable("添加设备...")) page = "devices";
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();
    if (!selected.empty() && page != "devices") {
        ImGui::PushFont(ui_heading_font(), 20.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
        if (ImGui::Selectable("概览", page == "overview", 0, {112, 36})) page = "overview";
        ImGui::SameLine();
        if (ImGui::Selectable("寄存器", page == "registers", 0, {112, 36})) page = "registers";
        ImGui::SameLine();
        if (ImGui::Selectable("插件", page == "plugins", 0, {112, 36})) { page = "plugins"; request_plugin_refresh(); }
        ImGui::PopStyleVar();
        ImGui::PopFont();
        ImGui::Separator();
    }
    const char *status = busy() ? job_label.c_str() :
        (notice.empty() ? (page == "registers" ? register_status.c_str() : "") : notice.c_str());
    const float status_height = ImGui::CalcTextSize(status, nullptr, false,
        ImGui::GetContentRegionAvail().x).y;
    const float footer_height = *status ? status_height + 2 * ImGui::GetStyle().ItemSpacing.y + 2 : 0;
    if (ImGui::BeginChild("##page", {0, -footer_height})) {
        if (page == "devices") draw_devices(*this);
        else if (page == "overview") draw_overview(*this);
        else if (page == "plugins") draw_plugins(*this);
        else draw_registers(*this);
    }
    ImGui::EndChild();
    if (*status) {
        ImGui::Separator();
        if (busy()) draw_loading(job_label.c_str());
        else if (!notice.empty()) ImGui::TextWrapped("%s", notice.c_str());
        else ImGui::TextDisabled("%s", status);
    }
    const bool uploading = busy() && job_label == "正在上传插件";
    if (uploading) ImGui::OpenPopup("上传插件");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
    if (ImGui::BeginPopupModal("上传插件", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (uploading) {
            draw_loading("正在上传插件");
            ImGui::Spacing();
            ImGui::TextDisabled("正在处理插件包，请稍候...");
        } else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}
}
