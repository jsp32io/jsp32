#include "font.hpp"
#include <fontconfig/fontconfig.h>
#include <imgui.h>
namespace jsp32 {
namespace {
ImFont *heading_font;

ImFont *load_font(const char *family, float size)
{
    FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(family));
    if (!pattern) return nullptr;
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
    FcChar8 *path = nullptr;
    ImFont *font = nullptr;
    if (match && FcPatternGetString(match, FC_FILE, 0, &path) == FcResultMatch)
        font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
            reinterpret_cast<const char *>(path), size, nullptr,
            ImGui::GetIO().Fonts->GetGlyphRangesChineseFull());
    if (match) FcPatternDestroy(match);
    FcPatternDestroy(pattern);
    return font;
}
}

void load_ui_font()
{
    FcInit();
    load_font("Noto Sans CJK SC", 18.0f);
    heading_font = load_font("Noto Sans CJK SC:style=Bold", 20.0f);
}
ImFont *ui_heading_font() { return heading_font; }
}
