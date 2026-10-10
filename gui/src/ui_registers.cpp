#include "app.hpp"
#include "font.hpp"
#include "api.hpp"
#include <imgui.h>
#include <algorithm>

namespace jsp32 {
void draw_registers(App &app)
{
    ImGui::PushFont(ui_heading_font(), 20.0f);
    ImGui::Text("寄存器");
    ImGui::PopFont();
    ImGui::Spacing();
    for (char area : {'X', 'Y', 'M', 'D'}) {
        ImGui::PushID(static_cast<int>(area));
        ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, {0.0f, 0.5f});
        if (ImGui::Selectable(std::string(1, area).c_str(), app.browse_area == area, 0, {42, 26})) {
            app.browse_area = area; app.browse_start = 0; app.register_scroll_reset = true;
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
        if (area != 'D') ImGui::SameLine();
    }
    ImGui::Spacing();
    unsigned count = register_area_count(app.register_info, app.browse_area);
    ImGui::BeginDisabled(app.browse_start < 32);
    if (ImGui::SmallButton("上一页")) { app.browse_start -= 32; app.register_scroll_reset = true; }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (count) ImGui::Text("%u – %u / %u", app.browse_start, std::min(app.browse_start + 31, count - 1), count);
    else ImGui::TextDisabled("—");
    ImGui::SameLine();
    ImGui::BeginDisabled(app.browse_start + 32 >= count);
    if (ImGui::SmallButton("下一页")) { app.browse_start += 32; app.register_scroll_reset = true; }
    ImGui::EndDisabled();
    if (ImGui::BeginTable("browse", 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_ScrollY, {0, -8})) {
        if (app.register_scroll_reset) { ImGui::SetScrollY(0); app.register_scroll_reset = false; }
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("地址", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("当前值");
        ImGui::TableHeadersRow();
        for (unsigned i = app.browse_start; i < std::min(app.browse_start + 32, count); ++i) {
            auto address = address_text(app.browse_area, i);
            ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(address.c_str());
            ImGui::TableNextColumn();
            auto it = app.values.find(address);
            if (it == app.values.end()) ImGui::TextDisabled("—");
            else ImGui::Text("%u", it->second);
        }
        ImGui::EndTable();
    }
}
}
