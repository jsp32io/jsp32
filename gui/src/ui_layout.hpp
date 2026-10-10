#pragma once

#include <imgui.h>
#include <algorithm>

namespace jsp32 {

inline void draw_loading(const char *label)
{
    const float size = ImGui::GetTextLineHeight();
    ImGui::Dummy({size, size});
    const auto origin = ImGui::GetItemRectMin();
    const float angle = static_cast<float>(ImGui::GetTime() * 4.0);
    auto *draw = ImGui::GetWindowDrawList();
    draw->PathArcTo({origin.x + size / 2, origin.y + size / 2}, size / 2 - 2,
                    angle, angle + 4.7f, 24);
    draw->PathStroke(ImGui::GetColorU32(ImGuiCol_Button), 0, 2.5f);
    ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
    ImGui::TextUnformatted(label);
}

inline float button_width(const char *label)
{
    return ImGui::CalcTextSize(label).x + 2 * ImGui::GetStyle().FramePadding.x;
}

// Call immediately after drawing a heading. Keep the actions on its line when
// they fit, and place them on the next line in a narrow window.
inline void align_actions_right(float width)
{
    const float start = ImGui::GetWindowContentRegionMax().x - width;
    const float heading_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    if (start >= heading_end + ImGui::GetStyle().ItemSpacing.x)
        ImGui::SameLine(start);
    else
        ImGui::SetCursorPosX(std::max(ImGui::GetWindowContentRegionMin().x, start));
}

}
