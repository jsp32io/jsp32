#include "app.hpp"
#include "font.hpp"
#include "api.hpp"
#include "ui_layout.hpp"
#include <imgui.h>
#include <imgui_stdlib.h>

namespace jsp32 {
void draw_devices(App &app)
{
    ImGui::Spacing();
    ImGui::PushFont(ui_heading_font(), 20.0f);
    ImGui::Text("选择设备");
    ImGui::PopFont();
    align_actions_right(button_width("+ 手动添加"));
    if (ImGui::Button("+ 手动添加")) ImGui::OpenPopup("手动添加设备");
    ImGui::TextDisabled("正在自动发现局域网中的 JSP32 设备");
    ImGui::Spacing();
    std::string remove_id;
    if (ImGui::BeginTable("devices", 5, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("设备", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthStretch, 2);
        ImGui::TableSetupColumn("固件版本", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableHeadersRow();
        for (const auto &device : app.discovered) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(device.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", device.url.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(device.version.empty() ? "-" : device.version.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted("在线");
            ImGui::TableNextColumn();
            ImGui::PushID(device.url.c_str());
            if (ImGui::SmallButton("连接") && !app.busy()) app.connect(device.url, device.name);
            ImGui::PopID();
        }
        for (const auto &device : app.settings.devices) {
            bool in_discovered = false;
            for (const auto &found : app.discovered) if (found.id == device.id) in_discovered = true;
            if (in_discovered) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(device.name.c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", device.url.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(device.version.empty() ? "-" : device.version.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted("离线");
            ImGui::TableNextColumn();
            ImGui::PushID(device.id.c_str());
            if (ImGui::SmallButton("进入")) app.select(device);
            ImGui::SameLine();
            if (ImGui::SmallButton("删除")) remove_id = device.id;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!remove_id.empty()) app.remove_device(remove_id);
    if (ImGui::BeginPopupModal("手动添加设备", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("输入设备 IP 地址或 URL");
        ImGui::SetNextItemWidth(340);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        bool enter = ImGui::InputTextWithHint("##manual", "192.168.1.20 或 http://192.168.1.20",
                                             &app.device_input, ImGuiInputTextFlags_EnterReturnsTrue);
        const bool can_connect = !app.busy() && !app.device_input.empty();
        ImGui::BeginDisabled(!can_connect);
        bool connect = ImGui::Button("连接设备", {110, 0});
        ImGui::EndDisabled();
        if ((connect || enter) && can_connect) {
            app.connect(app.device_input, "");
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", {110, 0})) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
}
