#include "pc_ui_theme.h"
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <string>

namespace pcui
{
static ImFont *display_font = nullptr;
void load_display_font()
{
    display_font = nullptr;
#if defined(_WIN32)
    const char *root = getenv("WINDIR");
    std::string candidate = std::string(root ? root : "C:/Windows") + "/Fonts/impact.ttf";
    const char *fonts[] = {candidate.c_str(), nullptr};
#elif defined(__APPLE__)
    const char *fonts[] = {"/System/Library/Fonts/Supplemental/Impact.ttf",
                           "/System/Library/Fonts/Supplemental/Arial Narrow Bold.ttf", nullptr};
#else
    const char *fonts[] = {"/usr/share/fonts/truetype/dejavu/DejaVuSansCondensed-Bold.ttf",
                           "/usr/share/fonts/TTF/DejaVuSansCondensed-Bold.ttf", nullptr};
#endif
    for (int i = 0; fonts[i]; i++)
    {
        std::error_code ec;
        if (std::filesystem::is_regular_file(fonts[i], ec))
        {
            display_font = ImGui::GetIO().Fonts->AddFontFromFileTTF(fonts[i], 22);
            if (display_font)
                break;
        }
    }
}
void theme(float scale)
{
    ImGuiStyle &s = ImGui::GetStyle();
    s = ImGuiStyle();
    ImGui::StyleColorsDark();
    s.WindowPadding = ImVec2(22, 18);
    s.FramePadding = ImVec2(12, 10);
    s.ItemSpacing = ImVec2(12, 10);
    s.WindowRounding = 0;
    s.ChildRounding = 5;
    s.FrameRounding = 3;
    s.GrabRounding = 3;
    s.TabRounding = 3;
    s.Colors[ImGuiCol_Text] = ImVec4(.914f, .875f, .784f, 1);
    s.Colors[ImGuiCol_TextDisabled] = ImVec4(.753f, .718f, .651f, 1);
    s.Colors[ImGuiCol_WindowBg] = ImVec4(.086f, .082f, .075f, 1);
    s.Colors[ImGuiCol_ChildBg] = ImVec4(.141f, .129f, .118f, 1);
    s.Colors[ImGuiCol_PopupBg] = ImVec4(.14f, .125f, .11f, 1);
    s.Colors[ImGuiCol_TitleBg] = ImVec4(.16f, .14f, .12f, 1);
    s.Colors[ImGuiCol_TitleBgActive] = ImVec4(.36f, .16f, .13f, 1);
    s.Colors[ImGuiCol_TitleBgCollapsed] = s.Colors[ImGuiCol_TitleBg];
    s.Colors[ImGuiCol_Border] = ImVec4(.318f, .294f, .255f, 1);
    s.Colors[ImGuiCol_Separator] = s.Colors[ImGuiCol_Border];
    s.Colors[ImGuiCol_FrameBg] = ImVec4(.098f, .09f, .082f, 1);
    s.Colors[ImGuiCol_FrameBgHovered] = ImVec4(.24f, .20f, .16f, 1);
    s.Colors[ImGuiCol_FrameBgActive] = ImVec4(.30f, .25f, .19f, 1);
    s.Colors[ImGuiCol_Button] = ImVec4(.188f, .169f, .145f, 1);
    s.Colors[ImGuiCol_ButtonHovered] = ImVec4(.30f, .25f, .19f, 1);
    s.Colors[ImGuiCol_ButtonActive] = ImVec4(.40f, .30f, .19f, 1);
    s.Colors[ImGuiCol_Header] = ImVec4(.36f, .16f, .13f, 1);
    s.Colors[ImGuiCol_HeaderHovered] = ImVec4(.48f, .21f, .17f, 1);
    s.Colors[ImGuiCol_HeaderActive] = ImVec4(.55f, .176f, .165f, 1);
    s.Colors[ImGuiCol_Tab] = ImVec4(.16f, .14f, .12f, 1);
    s.Colors[ImGuiCol_TabSelected] = ImVec4(141 / 255.f, 45 / 255.f, 42 / 255.f, 1);
    s.Colors[ImGuiCol_TabHovered] = ImVec4(.48f, .21f, .17f, 1);
    s.Colors[ImGuiCol_CheckboxSelectedBg] = ImVec4(.30f, .25f, .19f, 1);
    s.Colors[ImGuiCol_TabDimmed] = s.Colors[ImGuiCol_Tab];
    s.Colors[ImGuiCol_TabDimmedSelected] = s.Colors[ImGuiCol_TabSelected];
    s.Colors[ImGuiCol_TabSelectedOverline] = gold();
    s.Colors[ImGuiCol_TabDimmedSelectedOverline] = gold();
    s.Colors[ImGuiCol_TextSelectedBg] = ImVec4(.84f, .68f, .32f, .35f);
    s.Colors[ImGuiCol_CheckMark] = gold();
    s.Colors[ImGuiCol_SliderGrab] = gold();
    s.Colors[ImGuiCol_SliderGrabActive] = ImVec4(.94f, .77f, .41f, 1);
    s.Colors[ImGuiCol_NavCursor] = gold();
    s.ScaleAllSizes(scale);
    s.FontScaleMain = scale;
}
bool slider_int(const char *label, int *value, int minimum, int maximum)
{
    // Keep the full slider track visible against the page without brightening other fields.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(.24f, .22f, .19f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(.31f, .27f, .21f, 1));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(.37f, .31f, .23f, 1));
    bool changed = ImGui::SliderInt(label, value, minimum, maximum);
    ImGui::PopStyleColor(3);
    return changed;
}
void heading(const char *text, float size)
{
    ImGui::PushFont(display_font, size / ImGui::GetStyle().FontScaleMain);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
}
void brickwork(ImVec2 a, ImVec2 size, float s)
{
    auto *d = ImGui::GetWindowDrawList();
    d->PushClipRect(a, ImVec2(a.x + size.x, a.y + size.y), true);
    for (int row = 0; row < size.y / (46 * s) + 1; row++)
        for (int col = -1; col < size.x / (118 * s) + 1; col++)
        {
            float x = a.x + col * 118 * s + (row % 2 ? 59 * s : 0), y = a.y + row * 46 * s;
            d->AddRect(ImVec2(x, y), ImVec2(x + 118 * s, y + 46 * s), IM_COL32(115, 103, 80, 22));
        }
    d->PopClipRect();
}
void poster(ImVec2 a, ImVec2 size, float s)
{
    auto *d = ImGui::GetWindowDrawList();
    ImU32 paper = IM_COL32(233, 223, 200, 255), ink = IM_COL32(22, 21, 19, 255);
    float bottom = a.y + size.y;
    d->AddRectFilled(a, ImVec2(a.x + size.x, bottom - 10 * s), paper);
    for (int i = 0; i < 9; i++)
    {
        float x = a.x + size.x * i / 9.f, w = size.x / 9.f;
        d->AddTriangleFilled(ImVec2(x, bottom - 10 * s), ImVec2(x + w, bottom - 10 * s),
                             ImVec2(x + w * .42f, bottom - (i % 3) * 3 * s), paper);
    }
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, a.y + 16 * s));
    ImGui::PushStyleColor(ImGuiCol_Text, ink);
    heading("DEF JAM", 25 * s);
    d->AddLine(ImVec2(a.x + 18 * s, a.y + 51 * s), ImVec2(a.x + 110 * s, a.y + 51 * s), ink, 3 * s);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, a.y + 66 * s));
    float title = std::min({70 * s, (size.x - 36 * s) / 3.25f, (size.y - 122 * s) / 2.35f});
    heading("FIGHT", title);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, ImGui::GetCursorScreenPos().y));
    heading("FOR NY", title);
    ImGui::SetCursorScreenPos(ImVec2(a.x + 18 * s, bottom - 42 * s));
    ImGui::PushFont(nullptr, 11);
    ImGui::TextUnformatted("THE STREETS ARE WAITING.");
    ImGui::PopFont();
    ImGui::PopStyleColor();
}
bool play_button(const char *text, ImVec2 size, float font_size)
{
    ImGui::PushStyleColor(ImGuiCol_Button, gold());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(.93f, .77f, .42f, 1));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(.78f, .60f, .26f, 1));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(.086f, .075f, .05f, 1));
    ImGui::PushFont(display_font, font_size);
    bool hit = ImGui::Button(text, size);
    ImGui::PopFont();
    ImGui::PopStyleColor(4);
    return hit;
}
} // namespace pcui
