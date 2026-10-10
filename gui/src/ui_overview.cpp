#include "app.hpp"
#include "font.hpp"
#include "ui_layout.hpp"

#include <imgui.h>
#include <string>

namespace jsp32 {
namespace {
struct TxPowerOption {
    int qdbm;
    const char *label;
};

constexpr TxPowerOption tx_power_options[] = {
    {28, "7 dBm"}, {34, "8.5 dBm"}, {44, "11 dBm"},
    {52, "13 dBm"}, {56, "14 dBm"}, {60, "15 dBm"},
    {66, "16.5 dBm"}, {72, "18 dBm"}, {80, "20 dBm"},
};

std::string display_value(const Json::Value &value)
{
    if (value.isNull()) return "—";
    if (value.isBool()) return value.asBool() ? "是" : "否";
    if (value.isString()) return value.asString();
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    std::string text = Json::writeString(writer, value);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
    return text;
}

void draw_value(const std::string &name, const Json::Value &value)
{
    if (value.isObject() || value.isArray()) {
        const std::string label = name + (value.isArray() ? " (" + std::to_string(value.size()) + ")" : "");
        if (ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (value.isObject()) {
                for (const auto &key : value.getMemberNames()) {
                    if (key != "wifiMaxTxPowerQdbm") draw_value(key, value[key]);
                }
            } else {
                for (Json::ArrayIndex i = 0; i < value.size(); ++i)
                    draw_value(std::to_string(i + 1), value[i]);
            }
            ImGui::TreePop();
        }
        return;
    }
    ImGui::TextDisabled("%s", name.c_str());
    ImGui::SameLine(220);
    const auto text = display_value(value);
    ImGui::TextWrapped("%s", text.c_str());
}
}

void draw_overview(App &app)
{
    ImGui::PushFont(ui_heading_font(), 20.0f);
    ImGui::Text("设备概览");
    ImGui::PopFont();
    align_actions_right(button_width("刷新"));
    ImGui::BeginDisabled(app.busy());
    if (ImGui::Button("刷新")) app.refresh_overview();
    ImGui::EndDisabled();
    ImGui::Spacing();

    if (app.overview_info.isNull()) {
        ImGui::TextDisabled("正在读取设备信息...");
        return;
    }

    ImGui::SeparatorText("Wi-Fi 发射功率");
    const char *selected_label = "请选择档位";
    for (const auto &option : tx_power_options) {
        if (option.qdbm == app.tx_power_qdbm) selected_label = option.label;
    }
    ImGui::SetNextItemWidth(150);
    ImGui::BeginDisabled(app.busy());
    if (ImGui::BeginCombo("##tx_power", selected_label)) {
        for (const auto &option : tx_power_options) {
            if (ImGui::Selectable(option.label, app.tx_power_qdbm == option.qdbm)) {
                app.tx_power_qdbm = option.qdbm;
                const auto &current = app.overview_info["network"]["wifiMaxTxPowerQdbm"];
                app.tx_power_dirty = !current.isInt() || current.asInt() != option.qdbm;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (app.tx_power_dirty) {
        ImGui::SameLine();
        ImGui::BeginDisabled(app.busy());
        if (ImGui::Button("保存")) app.set_tx_power();
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("系统信息");
    for (const auto &key : app.overview_info.getMemberNames()) draw_value(key, app.overview_info[key]);
}
}
