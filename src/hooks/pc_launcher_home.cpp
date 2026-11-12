#include "pc_launcher_home.h"
#include "pc_ui_theme.h"
#include "pc_ui_actions.h"
#include "pc_ui_internal.h"
#include <algorithm>
#include <cstring>
#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif

UiHomeResult ui_launcher_home(float width, float height, const char *focus)
{
    UiHomeResult r;
    float s = ImGui::GetStyle().FontScaleMain, gap = 20 * s;
    bool split = width >= 850 * s;
    float tools = 65 * s, body = std::max(height - tools - 20 * s, 410 * s);
    if (split)
    {
        float left = width * .35f;
        ImGui::BeginChild("hero", ImVec2(left, body), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
        auto start = ImGui::GetCursorScreenPos();
        pcui::brickwork(start, ImVec2(left, body), s);
        ImGui::TextColored(pcui::gold(), "NEW YORK / 2004 / RECOMPILED");
        ImVec2 paper(start.x, start.y + 45 * s);
        float ph = std::min(300 * s, body - 175 * s);
        pcui::poster(paper, ImVec2(left - 10 * s, ph), s);
        ImGui::SetCursorScreenPos(ImVec2(start.x, paper.y + ph + 15 * s));
        ImGui::TextDisabled("FAN RECOMP / v%s", DEFJAM_VERSION);
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + body - 95 * s));
        ImGui::TextUnformatted("Ready for the next round");
        r.play = pcui::play_button("PLAY", ImVec2(left - 10 * s, 64 * s));
        ui_target("Play");
        if (ImGui::IsWindowAppearing())
            ImGui::SetItemDefaultFocus();
        ImGui::EndChild();
        ImGui::SameLine(0, gap);
        ImGui::BeginChild("quick settings", ImVec2(0, body),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    }
    else
    {
        pcui::heading("FIGHT FOR NY", 32 * s);
        ImGui::Spacing();
        ImGui::BeginChild("quick settings", ImVec2(0, 0),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                              ImGuiChildFlags_AlwaysUseWindowPadding);
    }
    pcui::heading("BEFORE YOU PLAY", 25 * s);
    ImGui::Spacing();
    ui_home_fields();
    ImGui::Separator();
    ui_home_devices();
    if (ImGui::Button("Configure controls"))
        r.route = 2;
    ui_target("Configure");
    if (focus && !strcmp(focus, "Configure"))
        ImGui::SetKeyboardFocusHere(-1);
    ImGui::EndChild();
    ImGui::Spacing();
    const char *labels[] = {"Advanced", "Mods", "Open Logs", "Open Folder", "Update / Repair"};
    int columns = std::max(1, std::min(5, (int)(width / (145 * s))));
    if (ImGui::BeginTable("utilities", columns, ImGuiTableFlags_SizingStretchSame))
    {
        for (int i = 0; i < 5; i++)
        {
            ImGui::TableNextColumn();
            bool hit = ImGui::Button(labels[i], ImVec2(-1, 44 * s));
            ui_target(labels[i]);
            if (focus && !strcmp(focus, labels[i]))
                ImGui::SetKeyboardFocusHere(-1);
            if (i == 0 && hit)
                r.route = 1;
            if (i == 1 && hit)
                r.route = 3;
            if (i == 2 && hit)
                pcui::open_path(pcui::folder_path(pcui::Folder::Logs));
            if (i == 3 && hit)
                ImGui::OpenPopup("folders");
            if (i == 3 && ImGui::BeginPopup("folders"))
            {
                const char *names[] = {"Data", "Saves", "Mods", "Installation", "Settings file"};
                pcui::Folder kinds[] = {pcui::Folder::Data, pcui::Folder::Saves, pcui::Folder::Mods,
                                        pcui::Folder::Install, pcui::Folder::Settings};
                for (int j = 0; j < 5; j++)
                    if (ImGui::MenuItem(names[j]))
                        pcui::open_path(pcui::folder_path(kinds[j]), j == 4);
                ImGui::EndPopup();
            }
            if (i == 4 && hit)
                r.route = 4;
        }
        ImGui::EndTable();
    }
    return r;
}
