#pragma once
struct UiHomeResult
{
    bool play = false;
    int route = 0;
}; // 1 Advanced, 2 Controls, 3 Mods, 4 Setup
UiHomeResult ui_launcher_home(float width, float height, const char *focus = nullptr);
void ui_home_fields();
void ui_home_devices();
