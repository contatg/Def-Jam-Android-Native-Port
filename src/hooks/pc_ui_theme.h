#pragma once
#include "imgui.h"
namespace pcui
{
inline ImVec4 gold()
{
    return ImVec4(214 / 255.f, 173 / 255.f, 82 / 255.f, 1);
}
void theme(float scale);
bool slider_int(const char *label, int *value, int minimum, int maximum);
void brickwork(ImVec2 start, ImVec2 size, float scale);
void poster(ImVec2 start, ImVec2 size, float scale);
void heading(const char *text, float size);
void load_display_font();
bool play_button(const char *text, ImVec2 size, float font_size = 30.0f);
} // namespace pcui
