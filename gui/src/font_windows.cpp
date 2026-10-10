#include "font.hpp"
#include <imgui.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <filesystem>
#include <initializer_list>
#include <vector>
namespace jsp32 {
namespace {
ImFont *heading_font;

ImFont *load_font(const std::filesystem::path &directory,
                  std::initializer_list<const wchar_t *> names,
                  std::vector<unsigned char> &bytes, float font_size)
{
    for (const wchar_t *name : names) {
        auto path = directory / L"Fonts" / name;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER file_size{};
        bool valid = GetFileSizeEx(file, &file_size) && file_size.QuadPart > 0 &&
                     file_size.QuadPart <= 64 * 1024 * 1024;
        if (valid) {
            bytes.resize(static_cast<size_t>(file_size.QuadPart));
            DWORD read = 0;
            valid = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
        }
        CloseHandle(file);
        if (!valid) continue;
        ImFontConfig config;
        config.FontDataOwnedByAtlas = false;
        if (auto *font = ImGui::GetIO().Fonts->AddFontFromMemoryTTF(
                bytes.data(), static_cast<int>(bytes.size()), font_size, &config,
                ImGui::GetIO().Fonts->GetGlyphRangesChineseFull())) return font;
    }
    return nullptr;
}
}

void load_ui_font()
{
    wchar_t directory[MAX_PATH];
    if (!GetWindowsDirectoryW(directory, MAX_PATH)) return;
    static std::vector<unsigned char> regular_bytes;
    static std::vector<unsigned char> bold_bytes;
    load_font(directory, {L"msyh.ttc", L"msyh.ttf", L"simhei.ttf"}, regular_bytes, 18.0f);
    heading_font = load_font(directory, {L"msyhbd.ttc", L"msyhbd.ttf", L"simhei.ttf"},
                             bold_bytes, 20.0f);
}
ImFont *ui_heading_font() { return heading_font; }
}
