// The settings screen, shared by the launcher and the in-game overlay, and the overlay itself.
//
// One screen edits settings.ini through the settings library, so a change made here is the value
// everywhere (the file, the hotkeys, the other front end), and is saved as it is made. The overlay
// draws over the picture in the presentation step (toolkit d3d8_present_set_overlay) and takes the
// keyboard and mouse from the window; while it is open the game is polled at rest. The launcher
// (pc_launcher.cpp) is the same screen in a window of its own before the game starts.
//
// Threads: the window's thread delivers input events, the thread that presents draws. One lock keeps
// them out of ImGui at the same time. Nothing here touches ImGui while the overlay is closed except
// to look for its hotkeys.

#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#include <d3d11.h>
#else
#include <spawn.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <mutex>
#include <SDL3/SDL.h>
#endif
#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <string>
#include <vector>
#include <map>
#include "pc_ui_model.h"
#include "pc_ui_theme.h"
#include "pc_ui_actions.h"
#include "pc_launcher_home.h"

#include "imgui.h"
#if defined(_WIN32)
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#else
#include "imgui_impl_vulkan.h"
#endif

extern "C" {
#include "recomp_settings.h"
#include "recorder.h"
#include "scene_launcher.h"
#include "dev_module.h"
#include "input_host.h"
#include "input_map.h"
}
#include "pc_input.h"
#include "pc_settings.h"
#include "pc_ui.h"
#include "pc_ui_internal.h"

#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "dev"
#endif

// ---- the lock -------------------------------------------------------------------------------

#if defined(_WIN32)
typedef ULONGLONG UiTick;
static UiTick ui_ticks() { return GetTickCount64(); }

static CRITICAL_SECTION g_cs;
static INIT_ONCE g_cs_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK init_cs(PINIT_ONCE, PVOID, PVOID *)
{
    InitializeCriticalSection(&g_cs);
    return TRUE;
}

void ui_lock()
{
    InitOnceExecuteOnce(&g_cs_once, init_cs, nullptr, nullptr);
    EnterCriticalSection(&g_cs);
}

void ui_unlock() { LeaveCriticalSection(&g_cs); }
#else
typedef unsigned long long UiTick;
static UiTick ui_ticks()
{
    using namespace std::chrono;
    return (UiTick)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Recursive, like a critical section: a setting changed inside the screen calls back into it.
static std::recursive_mutex g_mutex;
void ui_lock() { g_mutex.lock(); }
void ui_unlock() { g_mutex.unlock(); }
#endif

// ---- small helpers --------------------------------------------------------------------------

static RecompSetting *find(const char *section, const char *key)
{
    return recomp_settings_find(section, key);
}

static std::string get_text(const char *section, const char *key)
{
    char buf[RECOMP_SETTING_TEXT_MAX];
    recomp_settings_get_text(section, key, buf, sizeof(buf), "");
    return buf;
}

static void commit()
{
    pcui::clear_action_status();
    pc_settings_save();
}
static UiMode g_mode = UI_LAUNCHER;
struct UiTestTarget
{
    ImVec2 center;
    bool visible;
};
static std::map<std::string, UiTestTarget> g_targets;
static std::string g_reveal_target;
static std::string g_help;
void ui_target(const char *name)
{
    if (!getenv("RECOMP_LAUNCHER_SCRIPT"))
        return;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImVec2 center((a.x + b.x) * .5f, (a.y + b.y) * .5f);
    auto *draw = ImGui::GetWindowDrawList();
    ImVec2 lo = draw->GetClipRectMin(), hi = draw->GetClipRectMax();
    bool visible =
        ImGui::IsItemVisible() && center.x >= lo.x && center.y >= lo.y && center.x < hi.x && center.y < hi.y;
    g_targets[name] = {center, visible};
    if (g_reveal_target == name && !visible)
        ImGui::SetScrollHereY(.5f);
}
bool ui_test_target(const char *name, float *x, float *y)
{
    auto it = g_targets.find(name);
    if (it == g_targets.end() || !it->second.visible)
    {
        g_reveal_target = name;
        return false;
    }
    g_reveal_target.clear();
    *x = it->second.center.x;
    *y = it->second.center.y;
    return true;
}

static const char *k_control_labels[INPUT_CONTROL_COUNT] = {
    "D-pad up",
    "D-pad down",
    "D-pad left",
    "D-pad right",
    "Start",
    "Back",
    "Left stick click",
    "Right stick click",
    "A",
    "B",
    "X",
    "Y",
    "Black",
    "White",
    "Left trigger",
    "Right trigger",
    "Left stick up",
    "Left stick down",
    "Left stick left",
    "Left stick right",
    "Right stick up",
    "Right stick down",
    "Right stick left",
    "Right stick right",
};

static const char *friendly(const char *section, const char *key)
{
    struct Label { const char *section, *key, *text; };
    static const Label labels[] = {
        {"display", "fullscreen", "Full screen"},
        {"display", "window_width", "Window width"},
        {"display", "window_height", "Window height"},
        {"display", "aspect", "Picture shape"},
        {"display", "render_scale", "Render scale"},
        {"display", "filter", "Scaling filter"},
        {"display", "vsync", "Vsync"},
        {"display", "gamma", "Game brightness curve"},
        {"display", "brightness", "Brightness"},
        {"display", "vignette", "Vignette"},
        {"display", "vignette_strength", "Vignette strength (%)"},
        {"display", "widescreen_hud", "Fight HUD position"},
        {"display", "widescreen_cutscenes", "Story cutscenes"},
        {"display", "widescreen_side_fill", "Beside 4:3 pictures"},
        {"display", "widescreen_fov_cap", "Widest field of view (degrees)"},
        {"textures", "enabled", "Use HD texture packs"},
        {"textures", "packs", "Pack folders"},
        {"textures", "cache_mb", "Texture memory budget (MiB)"},
        {"textures", "dump", "Capture source textures"},
        {"textures", "dump_limit", "Maximum new captures"},
        {"input", "keyboard_player", "Keyboard and mouse play as"},
        {"input", "players", "Controllers at start"},
        {"input", "backend", "Pad backend"},
        {"input", "rumble", "Rumble strength"},
        {"input", "rumble_floor", "Weakest rumble pulse"},
        {"input", "rumble_min_ms", "Shortest rumble pulse (ms)"},
        {"input", "rumble_on_connect", "Buzz a pad when it connects"},
        {"input", "hide_cursor", "Hide the pointer when idle"},
        {"input", "confine_cursor", "Keep the pointer in the window (full screen)"},
        {"gamepad", "left_deadzone", "Left stick dead zone"},
        {"gamepad", "right_deadzone", "Right stick dead zone"},
        {"gamepad", "trigger_threshold", "Trigger threshold"},
        {"gamepad", "button_threshold", "Button threshold"},
        {"gamepad", "left_stick", "Left stick comes from"},
        {"gamepad", "right_stick", "Right stick comes from"},
        {"launcher", "skip", "Skip the launcher at start"},
        {"ui", "scale", "Text and control size"},
        {"debug", "unlock_all", "Unlock all fighters and venues"},
        {"debug", "skip_intro", "Skip startup movies and legal screen"},
        {"ui", "overlay_pad", "Overlay pad buttons"},
        {"ui", "overlay_key", "Overlay key"},
        {"debug", "record", "Record play-tests"},
        {"debug", "mark_key", "Mark key"},
        {"debug", "mark_pad", "Mark pad buttons"},
        {"debug", "record_fps", "Pictures kept a second"},
        {"debug", "record_seconds", "Seconds kept before a mark"},
        {"debug", "record_audio", "Keep the sound"},
        {"debug", "record_game_state", "Log fighters and match events"},
        {"debug", "auto_bundle_hitch_ms", "Bundle a hitch longer than (ms)"},
        {"debug", "hang_seconds", "Bundle a hang after (s)"},
        {"debug", "keep_sessions", "Sessions kept whole"},
        {"debug", "record_guest_ram", "Crash bundles hold game memory"},
    };
    for (const Label &l : labels)
        if (!strcmp(l.section, section) && !strcmp(l.key, key)) return l.text;
    return key;
}

// ---- restart-needed tracking ----------------------------------------------------------------

struct StartValue { std::string section, key, text; };
static std::vector<StartValue> g_start_values;

static std::string value_text(const RecompSetting *s)
{
    char buf[RECOMP_SETTING_TEXT_MAX];
    return recomp_settings_format(s, s->stored, buf, sizeof(buf));
}

static void remember_start_values()
{
    if (!g_start_values.empty()) return;
    for (size_t i = 0; i < recomp_settings_count(); i++) {
        RecompSetting *s = recomp_settings_at(i);
        if ((s->flags & RECOMP_SETTING_RESTART) ||
            (!strcmp(s->section, "display") && !strcmp(s->key, "aspect")))
            g_start_values.push_back({ s->section, s->key, value_text(s) });
    }
}

static std::string restart_needed()
{
    std::string out;
    for (const StartValue &v : g_start_values) {
        RecompSetting *s = find(v.section.c_str(), v.key.c_str());
        if (s && value_text(s) != v.text) {
            if (v.key == "aspect")
            {
                int before = 0;
                for (int i = 0; s->choices[i]; i++)
                    if (v.text == s->choices[i])
                        before = i;
                if (!pcui::aspect_next_launch(before, s->stored))
                    continue;
            }
            if (!out.empty()) out += ", ";
            out += friendly(v.section.c_str(), v.key.c_str());
        }
    }
    return out;
}

// ---- learning a key or a pad button ------------------------------------------------------------

namespace {

struct Capture {
    enum Kind { NONE, KEY, PAD } kind = NONE;
    std::string section, key;       // the setting that gets the result
    bool append = false;            // keyboard bindings take up to four keys; others take one
    UiTick armed_at = 0;
};

Capture g_capture;
bool g_pad_prev[INPUT_SRC_COUNT];

void capture_start(Capture::Kind kind, const char *section, const char *key, bool append)
{
    g_capture.kind = kind;
    g_capture.section = section;
    g_capture.key = key;
    g_capture.append = append;
    g_capture.armed_at = ui_ticks() + 250;     // the click that started it is not the answer
    memset(g_pad_prev, 1, sizeof(g_pad_prev));       // a button already down does not count
}

void capture_cancel() { g_capture.kind = Capture::NONE; }

void capture_commit_key(int vk)
{
    const char *raw_name = input_key_name(vk);
    if (!raw_name) return;
    std::string name = raw_name;
    if (name.size() > 1) name[0] = (char)toupper((unsigned char)name[0]);     // "space" -> "Space"
    std::string text = name;
    if (g_capture.append) {
        std::string old = get_text(g_capture.section.c_str(), g_capture.key.c_str());
        InputBindings b;
        memset(&b, 0, sizeof(b));
        input_bindings_parse(&b, 0, old.c_str());
        for (int i = 0; i < b.count[0]; i++)
            if (b.key[0][i] == vk) { capture_cancel(); return; }       // already there
        if (b.count[0] >= INPUT_BINDINGS_PER_CONTROL) { capture_cancel(); return; }
        if (!old.empty() && old != "none") text = old + ", " + name;
    }
    recomp_settings_set_text(g_capture.section.c_str(), g_capture.key.c_str(), text.c_str());
    commit();
    capture_cancel();
}

} // namespace

bool ui_capturing() { return g_capture.kind != Capture::NONE; }

#if defined(_WIN32)
bool ui_capture_message(HWND, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_capture.kind != Capture::KEY) return false;
    bool armed = ui_ticks() >= g_capture.armed_at;
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        int vk = pc_input_vk_from_message(wp, lp);
        if (vk == VK_ESCAPE) { capture_cancel(); return true; }
        if (armed && vk != VK_F11 && vk != VK_MENU) capture_commit_key(vk);
        return true;
    }
    case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
        return true;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN:     // clicks are the menu's: the left and right buttons have buttons of their own
        return false;
    case WM_MBUTTONDOWN: if (armed) capture_commit_key(VK_MBUTTON); return true;
    case WM_XBUTTONDOWN:
        if (armed) capture_commit_key(HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2);
        return true;
    case WM_MOUSEWHEEL:
        if (armed) capture_commit_key(GET_WHEEL_DELTA_WPARAM(wp) > 0 ? INPUT_KEY_WHEEL_UP : INPUT_KEY_WHEEL_DOWN);
        return true;
    }
    return false;
}
#else
bool ui_capture_sdl_event(const SDL_Event *e)
{
    if (g_capture.kind != Capture::KEY) return false;
    bool armed = ui_ticks() >= g_capture.armed_at;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        int vk = host_posix_vk_from_scancode(e->key.scancode);
        if (vk == VK_ESCAPE) { capture_cancel(); return true; }
        if (armed && vk && !e->key.repeat && vk != VK_F11 && vk != VK_MENU && vk != 0xA4 && vk != 0xA5)
            capture_commit_key(vk);
        return true;
    }
    case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // Clicks are the menu's: the left and right buttons have buttons of their own.
        if (e->button.button == SDL_BUTTON_MIDDLE) { if (armed) capture_commit_key(VK_MBUTTON); return true; }
        if (e->button.button == SDL_BUTTON_X1) { if (armed) capture_commit_key(VK_XBUTTON1); return true; }
        if (e->button.button == SDL_BUTTON_X2) { if (armed) capture_commit_key(VK_XBUTTON2); return true; }
        return false;
    case SDL_EVENT_MOUSE_WHEEL:
        if (armed && e->wheel.y != 0)
            capture_commit_key(e->wheel.y > 0 ? INPUT_KEY_WHEEL_UP : INPUT_KEY_WHEEL_DOWN);
        return true;
    }
    return false;
}
#endif

// Pads: the first button pressed on any of them becomes the answer.
static void capture_poll_pad()
{
    if (g_capture.kind != Capture::PAD) return;
    for (int slot = 0; slot < 4; slot++) {
        InputRaw raw;
        if (!xbox_HostInputRawPad(slot, &raw)) continue;
        for (int s = INPUT_SRC_SOUTH; s < INPUT_SRC_COUNT; s++) {
            bool down = raw.src[s] >= 16384;
            if (down && !g_pad_prev[s] && ui_ticks() >= g_capture.armed_at) {
                recomp_settings_set_text(g_capture.section.c_str(), g_capture.key.c_str(), input_source_name(s));
                commit();
                capture_cancel();
                return;
            }
            g_pad_prev[s] = down;
        }
    }
}

// ---- pads drive the menu --------------------------------------------------------------------------

void ui_feed_gamepad()
{
    ImGuiIO &io = ImGui::GetIO();
    InputRaw all;
    memset(&all, 0, sizeof(all));
    for (int slot = 0; slot < 4; slot++) {
        InputRaw raw;
        if (!xbox_HostInputRawPad(slot, &raw)) continue;
        for (int s = 0; s < INPUT_SRC_COUNT; s++)
            if (raw.src[s] > all.src[s]) all.src[s] = raw.src[s];
    }
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    auto key = [&](ImGuiKey k, int src) { io.AddKeyEvent(k, all.src[src] >= 16384); };
    auto analog = [&](ImGuiKey k, int src) {
        float v = all.src[src] / 32767.0f;
        io.AddKeyAnalogEvent(k, v > 0.25f, v > 0.25f ? v : 0.0f);
    };
    // Detecting a button for a binding must not also press the menu.
    bool detecting = g_capture.kind == Capture::PAD;
    if (detecting) {
        for (int k = ImGuiKey_GamepadStart; k <= ImGuiKey_GamepadRStickRight; k++) io.AddKeyEvent((ImGuiKey)k, false);
        return;
    }
    key(ImGuiKey_GamepadDpadUp, INPUT_SRC_DPAD_UP);      key(ImGuiKey_GamepadDpadDown, INPUT_SRC_DPAD_DOWN);
    key(ImGuiKey_GamepadDpadLeft, INPUT_SRC_DPAD_LEFT);  key(ImGuiKey_GamepadDpadRight, INPUT_SRC_DPAD_RIGHT);
    key(ImGuiKey_GamepadFaceDown, INPUT_SRC_SOUTH);      key(ImGuiKey_GamepadFaceRight, INPUT_SRC_EAST);
    key(ImGuiKey_GamepadFaceLeft, INPUT_SRC_WEST);       key(ImGuiKey_GamepadFaceUp, INPUT_SRC_NORTH);
    key(ImGuiKey_GamepadL1, INPUT_SRC_LEFT_SHOULDER);    key(ImGuiKey_GamepadR1, INPUT_SRC_RIGHT_SHOULDER);
    key(ImGuiKey_GamepadStart, INPUT_SRC_START);         key(ImGuiKey_GamepadBack, INPUT_SRC_BACK);
    analog(ImGuiKey_GamepadL2, INPUT_SRC_LEFT_TRIGGER);  analog(ImGuiKey_GamepadR2, INPUT_SRC_RIGHT_TRIGGER);
    analog(ImGuiKey_GamepadLStickUp, INPUT_SRC_LSTICK_UP);     analog(ImGuiKey_GamepadLStickDown, INPUT_SRC_LSTICK_DOWN);
    analog(ImGuiKey_GamepadLStickLeft, INPUT_SRC_LSTICK_LEFT); analog(ImGuiKey_GamepadLStickRight, INPUT_SRC_LSTICK_RIGHT);
}

// ---- context, fonts, style -----------------------------------------------------------------------

// Beyond the display's own scaling, a window much bigger than the game's 1280x960 default (full screen
// on a large monitor) gets bigger text, so the screen is as easy to read as it is in a window.
static float g_extra_scale = 1.0f;

static float display_scale(UiWindow window)
{
    float dpi = g_extra_scale;
#if defined(_WIN32)
    if (window) {
        UINT d = GetDpiForWindow(window);
        if (d) dpi *= d / 96.0f;
    }
#else
    // The window's points are not its pixels on a Retina display, and the screen is drawn in points
    // (the launcher) or already sized by the picture's height (the overlay): nothing to add.
    (void)window;
#endif
    int percent = recomp_settings_get("ui", "scale", 100);
    return dpi * (percent / 100.0f);
}

static float g_scale_applied;

void ui_apply_scale(UiWindow window)
{
    float s = display_scale(window);
    float &applied = g_scale_applied;
    ImGuiStyle &style = ImGui::GetStyle();
    if (s == applied) return;
    applied = s;
    pcui::theme(s);
}

void ui_context_begin(UiWindow window)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    pcui::clear_action_status();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.ConfigNavCaptureKeyboard = true;
    g_scale_applied = 0.0f;                 // a new context has not been styled yet
    // The system's UI font reads better than the built-in one at small sizes.
#if defined(_WIN32)
    char font[MAX_PATH];
    if (GetWindowsDirectoryA(font, sizeof(font))) {
        strcat(font, "\\Fonts\\segoeui.ttf");
        if (GetFileAttributesA(font) != INVALID_FILE_ATTRIBUTES)
            io.Fonts->AddFontFromFileTTF(font, 17.0f);
    }
#else
    static const char *const fonts[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Helvetica.ttc",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    };
    struct stat st;
    for (const char *font : fonts)
        if (stat(font, &st) == 0 && io.Fonts->AddFontFromFileTTF(font, 17.0f)) break;
#endif
    pcui::load_display_font();
    ui_apply_scale(window);
    remember_start_values();
}

void ui_context_end()
{
    ImGui::DestroyContext();
    g_capture.kind = Capture::NONE;
    g_start_values.clear();         // what was changed in the launcher has been applied: the overlay starts afresh
}

// ---- generic setting widgets --------------------------------------------------------------------------

// A hover tooltip for the last item, wrapped: ImGui's own one-line tooltip does not wrap, and the longer
// help texts ran past the right edge of the window and were cut off.
static void item_tooltip(const char *text)
{
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        return;
    const auto &style = ImGui::GetStyle();
    ImVec2 area = ImGui::GetIO().DisplaySize;
    float margin = 8 * style.FontScaleMain;
    float font_size = ImGui::GetFontSize(), minimum_font = std::min(font_size, 17.0f);
    float wrap;
    ImVec2 text_size;
    for (;;)
    {
        ImGui::PushFont(nullptr, font_size / style.FontScaleMain);
        wrap = std::max(1.0f, std::min(ImGui::GetFontSize() * 35.0f,
                                     area.x - 2 * (margin + style.WindowPadding.x)));
        text_size = ImGui::CalcTextSize(text, nullptr, false, wrap);
        if (text_size.y + 2 * style.WindowPadding.y <= area.y - 2 * margin || font_size <= minimum_font)
            break;
        ImGui::PopFont();
        font_size = std::max(minimum_font, font_size * .9f);
    }
    ImVec2 size(text_size.x + 2 * style.WindowPadding.x, text_size.y + 2 * style.WindowPadding.y);
    ImVec2 mouse = ImGui::GetIO().MousePos;
    // ImGui may place a tall tooltip outside the viewport when no side of the pointer fits it.
    ImGui::SetNextWindowPos(ImVec2(
        std::clamp(mouse.x + 12 * style.FontScaleMain, margin, std::max(margin, area.x - margin - size.x)),
        std::clamp(mouse.y + 20 * style.FontScaleMain, margin, std::max(margin, area.y - margin - size.y))));
    if (!ImGui::BeginTooltip())
    {
        ImGui::PopFont();
        return;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopFont();
}

// A bool, integer, choice or text setting as its natural control. Returns true when it changed.
static bool setting_widget(RecompSetting *s, const char *label = nullptr, bool stacked = false)
{
    if (!s)
        return false;
    if (!label)
        label = friendly(s->section, s->key);
    // An explicit pack list enables/disables texture replacements independently of the file switch.
    RecompSetting *packs = find("textures", "packs");
    bool pack_override = !strcmp(s->section, "textures") && !strcmp(s->key, "enabled") && packs &&
                         packs->source == RECOMP_SETTING_FROM_ENV;
    bool quick_field = stacked;
    stacked = stacked || ImGui::GetContentRegionAvail().x < ImGui::GetFontSize() * 36;
    bool locked = s->source == RECOMP_SETTING_FROM_ENV || pack_override, changed = false;
    int value = pack_override ? packs->value_text[0] != 0 : locked ? s->value : s->stored;
    ImGui::PushID(s);
    const char *control = stacked ? "##value" : label;
    if (stacked)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", label);
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(-1);
    }
    if (locked)
        ImGui::BeginDisabled();
    switch (s->type)
    {
    case RECOMP_SETTING_BOOL:
        if (stacked)
        {
            const char *off = !strcmp(s->key, "fullscreen") ? "Windowed" : "Off";
            const char *on = !strcmp(s->key, "fullscreen") ? "Borderless fullscreen" : "On";
            if (ImGui::BeginCombo(control, value ? on : off))
            {
                for (int i = 0; i < 2; i++)
                    if (ImGui::Selectable(i ? on : off, value == i))
                        changed = recomp_settings_set(s->section, s->key, i) > 0;
                ImGui::EndCombo();
            }
        }
        else
        {
            bool v = value != 0;
            if (ImGui::Checkbox(control, &v))
                changed = recomp_settings_set(s->section, s->key, v) > 0;
        }
        break;
    case RECOMP_SETTING_INT: {
        if (!stacked)
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        int v = value;
        if (stacked && !strcmp(s->key, "render_scale"))
        {
            char current[24];
            snprintf(current, sizeof current, "%dx%s", v, v == 2 ? " (default)" : "");
            if (ImGui::BeginCombo(control, current))
            {
                for (int i = 1; i <= 4; i++)
                {
                    char item[24];
                    snprintf(item, sizeof item, "%dx%s", i, i == 2 ? " (default)" : "");
                    if (ImGui::Selectable(item, v == i))
                        changed = recomp_settings_set(s->section, s->key, i) > 0;
                }
                ImGui::EndCombo();
            }
        }
        else if (s->max - s->min > 2000)
        {
            if (ImGui::InputInt(control, &v, 10, 100))
                changed = recomp_settings_set(s->section, s->key, v) > 0;
        }
        else if (pcui::slider_int(control, &v, s->min, s->max))
            changed = recomp_settings_set(s->section, s->key, v) > 0;
        break;
    }
    case RECOMP_SETTING_ENUM:
        if (!stacked)
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
        if (ImGui::BeginCombo(control, s->choices[value]))
        {
            for (int i = 0; s->choices[i]; i++)
                if (ImGui::Selectable(s->choices[i], i == value))
                    changed = recomp_settings_set(s->section, s->key, i) > 0;
            ImGui::EndCombo();
        }
        break;
    case RECOMP_SETTING_STRING: {
        char buf[RECOMP_SETTING_TEXT_MAX];
        snprintf(buf, sizeof buf, "%s", locked ? s->value_text : s->stored_text);
        if (!stacked)
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22);
        if (ImGui::InputText(control, buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue))
            changed = recomp_settings_set_text(s->section, s->key, buf) > 0;
        break;
    }
    }
    std::string id = std::string(s->section) + "." + s->key;
    ui_target(id.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())
        g_help = s->help ? s->help : label;
    if (locked)
        ImGui::EndDisabled();
    if (s->help)
        item_tooltip(s->help);
    if (locked)
    {
        ImGui::TextDisabled("Set for this run by %s", pack_override ? "RECOMP_TEXTURE_PACKS"
                                                      : s->env      ? s->env
                                                                    : "the environment");
    }
    else if (!quick_field && (s->flags & RECOMP_SETTING_RESTART))
    {
        if (!stacked)
            ImGui::SameLine();
        ImGui::TextDisabled(g_mode == UI_LAUNCHER ? "(on Play)" : "(next launch)");
    }
    ImGui::PopID();
    if (changed)
        commit();
    return changed;
}

static void settings_group(const char *section, std::initializer_list<const char *> keys)
{
    for (const char *key : keys) {
        RecompSetting *s = find(section, key);
        if (s) setting_widget(s);
    }
}

// ---- the pages ----------------------------------------------------------------------------------------------

static void page_display()
{
    ImGui::SeparatorText("Window and picture");
    settings_group("display", { "fullscreen", "window_width", "window_height", "aspect" });
    if (recomp_settings_get("display", "aspect", 0) >= 2) {
        /* A widescreen shape: its own options, under it. */
        ImGui::Indent();
        settings_group("display", { "widescreen_hud", "widescreen_cutscenes", "widescreen_side_fill",
                                    "widescreen_fov_cap" });
        ImGui::TextWrapped("Widescreen (beta): the 3D view gets wider; menus, the HUD and movies keep their shape. "
                           "widescreen (auto) matches your screen (16:9 or 16:10). It starts with the game: choose it "
                           "here before Play, or restart after changing it in the overlay.");
        ImGui::Unindent();
    }
    settings_group("display", { "filter", "vsync" });
    ImGui::SeparatorText("Quality");
    settings_group("display", { "render_scale" });
    ImGui::SeparatorText("Picture");
    settings_group("display", { "brightness", "gamma", "vignette" });
    if (recomp_settings_get("display", "vignette", 1)) {
        ImGui::Indent();
        settings_group("display", { "vignette_strength" });
        ImGui::Unindent();
    }
    ImGui::TextWrapped("Full screen, window size, filter, vsync, the 4:3 and stretch shapes, brightness and the vignette "
                       "apply as you change them. The widescreen shapes, render scale and the brightness curve apply the "
                       "next time the game starts. F11 or Alt+Enter switches full screen while playing.");
}

static std::vector<pcui::Pack> g_packs;
static std::string g_pack_key, g_pack_path, g_pack_status;
static void refresh_packs(bool force = false)
{
    std::string key = get_text("textures", "packs"), path = pcui::folder_path(pcui::Folder::Mods);
    if (force || key != g_pack_key || path != g_pack_path)
    {
        g_pack_key = key;
        g_pack_path = path;
        g_packs = pcui::inventory(std::filesystem::u8path(path), key);
    }
}
static bool packs_available()
{
    refresh_packs();
    for (auto &p : g_packs)
        if (p.selected && p.problem.empty())
            return true;
    return false;
}
static void save_packs()
{
    auto text = pcui::selected_packs(g_packs);
    if (text.size() >= RECOMP_SETTING_TEXT_MAX)
    {
        g_pack_status = "Pack list is too long; change was not saved.";
        g_pack_path.clear(); // Reload inventory next frame, even when the saved list is empty.
        return;
    }
    recomp_settings_set_text("textures", "packs", text.c_str());
    commit();
    g_pack_key = text;
    g_pack_status = "Saved. Later packs take priority; applies at next launch.";
}
static void page_textures()
{
    ImGui::SeparatorText("Local texture packs");
    settings_group("textures", {"enabled"});
    refresh_packs();
    bool locked = find("textures", "packs")->source == RECOMP_SETTING_FROM_ENV;
    if (locked)
        ImGui::TextWrapped("Pack selection is set for this run by RECOMP_TEXTURE_PACKS.");
    if (ImGui::Button("Refresh packs"))
        refresh_packs(true);
    ImGui::SameLine();
    if (ImGui::Button("Open Mods Folder"))
        pcui::open_path(g_pack_path);
    ImGui::TextWrapped("Later selected packs take priority. Pack changes apply next launch.");
    if (std::none_of(g_packs.begin(), g_packs.end(), [](const pcui::Pack &p) { return p.problem.empty(); }))
        ImGui::TextWrapped(
            "No local packs found. Setup can generate the faithful HD packs from your own dump.");
    ImGui::BeginDisabled(locked);
    for (size_t i = 0; i < g_packs.size(); i++)
    {
        auto &p = g_packs[i];
        const std::string pack_id = p.name;
        ImGui::PushID((int)i);
        bool checked = p.selected;
        ImGui::BeginDisabled(!p.problem.empty() && !p.selected);
        if (ImGui::Checkbox(p.name.c_str(), &checked))
        {
            p.selected = checked;
            save_packs();
        }
        ui_target(("pack." + pack_id).c_str());
        ImGui::EndDisabled();
        if (p.selected)
        {
            ImGui::SameLine();
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton("Earlier"))
            {
                std::swap(g_packs[i], g_packs[i - 1]);
                save_packs();
            }
            ui_target(("pack.earlier." + pack_id).c_str());
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == g_packs.size());
            if (ImGui::SmallButton("Later"))
            {
                std::swap(g_packs[i], g_packs[i + 1]);
                save_packs();
            }
            ui_target(("pack.later." + pack_id).c_str());
            ImGui::EndDisabled();
        }
        if (!p.problem.empty())
            ImGui::TextWrapped("%s", p.problem.c_str());
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!g_pack_status.empty())
        ImGui::TextWrapped("%s", g_pack_status.c_str());
    if (ImGui::CollapsingHeader("Memory and manual pack list"))
        settings_group("textures", {"cache_mb", "packs"});
}
void ui_home_fields()
{
    bool two = ImGui::GetContentRegionAvail().x > 390 * ImGui::GetStyle().FontScaleMain;
    auto begin = [](const char *id) {
        ImGui::TableNextColumn();
        ImGui::BeginChild(id, ImVec2(0, 80 * ImGui::GetStyle().FontScaleMain), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    };
    if (ImGui::BeginTable("quick fields", two ? 2 : 1, ImGuiTableFlags_SizingStretchSame))
    {
        begin("mode");
        setting_widget(find("display", "fullscreen"), "Display mode", true);
        ImGui::EndChild();
        begin("size");
        ImGui::TextDisabled("Window size");
        int w = pc_display("window_width", 1280), h = pc_display("window_height", 960);
        char label[48];
        snprintf(label, sizeof label, "%d x %d", w, h);
        bool locked = find("display", "window_width")->source == RECOMP_SETTING_FROM_ENV ||
                      find("display", "window_height")->source == RECOMP_SETTING_FROM_ENV;
        ImGui::BeginDisabled(pc_display("fullscreen", 0) != 0 || locked);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##window size", label))
        {
            const int sizes[][2] = {{1280, 960}, {1280, 720}, {1280, 800}, {1920, 1080}, {2560, 1440}};
            bool custom = true;
            for (auto &size : sizes)
            {
                char name[48];
                snprintf(name, sizeof name, "%d x %d", size[0], size[1]);
                bool selected = w == size[0] && h == size[1];
                custom &= !selected;
                if (ImGui::Selectable(name, selected))
                {
                    pc_display_set("window_width", size[0]);
                    pc_display_set("window_height", size[1]);
                }
            }
            if (custom)
                ImGui::TextDisabled("Custom: %s (kept)", label);
            ImGui::EndCombo();
        }
        ui_target("display.window_size");
        ImGui::EndDisabled();
        ImGui::EndChild();
        begin("aspect");
        setting_widget(find("display", "aspect"), "Picture shape", true);
        ImGui::EndChild();
        begin("render_scale");
        setting_widget(find("display", "render_scale"), "Render scale", true);
        ImGui::EndChild();
        begin("vsync");
        setting_widget(find("display", "vsync"), "VSync", true);
        ImGui::EndChild();
        begin("textures");
        bool available = packs_available();
        ImGui::BeginDisabled(!available && !recomp_settings_get("textures", "enabled", 0));
        setting_widget(find("textures", "enabled"), available ? "HD textures" : "HD textures (not installed)",
                       true);
        ImGui::EndDisabled();
        ImGui::EndChild();
        ImGui::EndTable();
    }
}
void ui_home_devices()
{
    bool any = false;
    for (int i = 0; i < 4; i++)
    {
        char name[64];
        if (xbox_HostInputSlotInfo(i, name, sizeof name))
        {
            ImGui::TextWrapped("Player %d: %s", i + 1, name);
            any = true;
        }
    }
    if (!any)
        ImGui::TextDisabled("No controller connected / keyboard available");
}

static void draw_stick(const char *id, float x, float y, float radius)
{
    ImVec2 c = ImGui::GetCursorScreenPos();
    c.x += radius; c.y += radius;
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircle(c, radius, IM_COL32(150, 150, 150, 255), 24, 2.0f);
    dl->AddCircleFilled(ImVec2(c.x + x * radius * 0.9f, c.y - y * radius * 0.9f), radius * 0.16f, IM_COL32(255, 190, 50, 255));
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    (void)id;
}

static void page_controllers()
{
    ImGui::SeparatorText("Players");
    InputPadMap map;
    pc_input_current_padmap(&map);
    for (int slot = 0; slot < 4; slot++) {
        char name[64];
        int what = xbox_HostInputSlotInfo(slot, name, sizeof(name));
        ImGui::PushID(slot);
        ImGui::Text("Player %d:", slot + 1);
        ImGui::SameLine();
        if (!what) { ImGui::TextDisabled("nothing connected"); ImGui::PopID(); continue; }
        ImGui::TextUnformatted(name);
        InputRaw raw;
        if (what & 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Test rumble")) xbox_HostInputRumbleTest(slot);
            if (xbox_HostInputRawPad(slot, &raw)) {
                InputPad pad;
                input_pad_map(&map, &raw, &pad);
                ImGui::Indent();
                draw_stick("l", pad.lx / 32767.0f, pad.ly / 32767.0f, ImGui::GetFontSize() * 1.6f);
                ImGui::SameLine();
                draw_stick("r", pad.rx / 32767.0f, pad.ry / 32767.0f, ImGui::GetFontSize() * 1.6f);
                ImGui::SameLine();
                static const char *btn[8] = { "A", "B", "X", "Y", "Black", "White", "LT", "RT" };
                ImGui::BeginGroup();
                std::string pressed;
                for (int i = 0; i < 8; i++)
                    if (pad.analog[i] > 30) { pressed += btn[i]; pressed += ' '; }
                static const char *dig[8] = { "Up", "Down", "Left", "Right", "Start", "Back", "LS", "RS" };
                for (int i = 0; i < 8; i++)
                    if (pad.buttons & (1 << i)) { pressed += dig[i]; pressed += ' '; }
                ImGui::TextDisabled("what the game sees:");
                ImGui::TextUnformatted(pressed.empty() ? "-" : pressed.c_str());
                ImGui::EndGroup();
                ImGui::Unindent();
            }
        }
        ImGui::PopID();
    }
    ImGui::TextWrapped("Press buttons and push the sticks to check them. The circles show the sticks after the dead zones below.");

    ImGui::SeparatorText("Who plays");
    settings_group("input", { "keyboard_player", "players", "backend" });
    ImGui::SeparatorText("Sticks and triggers");
    settings_group("gamepad", { "left_deadzone", "right_deadzone", "trigger_threshold", "button_threshold",
                                "left_stick", "right_stick" });
    ImGui::SeparatorText("Rumble");
    settings_group("input", { "rumble", "rumble_floor", "rumble_min_ms", "rumble_on_connect" });
    ImGui::SeparatorText("Pointer");
    settings_group("input", { "hide_cursor", "confine_cursor" });
}

static void page_pad_buttons()
{
    ImGui::TextWrapped("Which physical button acts as each Xbox control. Choose from the list, or press Detect and then the "
                       "button on your pad. Names are positional: south is the bottom face button (Xbox A, PlayStation cross), "
                       "east the right one, west the left, north the top.");
    if (ImGui::Button("Reset all to default")) {
        for (int i = 0; i < INPUT_PAD_CONTROL_COUNT; i++) recomp_settings_reset_to_default("gamepad", input_control_name(i));
        commit();
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("padmap", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Xbox control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Pad button", ImGuiTableColumnFlags_WidthStretch, 1.6f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < INPUT_PAD_CONTROL_COUNT; i++) {
            const char *key = input_control_name(i);
            RecompSetting *s = find("gamepad", key);
            if (!s) continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(k_control_labels[i]);
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            int cur = input_source_from_name(s->stored_text);
            if (cur < 0) cur = 0;
            bool detecting = g_capture.kind == Capture::PAD && g_capture.key == key;
            ImGui::SetNextItemWidth(-1);
            if (detecting) {
                ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "Press a button on your pad...");
            } else if (ImGui::BeginCombo("##src", input_source_name(cur))) {
                for (int n = 0; n < INPUT_SRC_COUNT; n++)
                    if (ImGui::Selectable(input_source_name(n), n == cur)) {
                        recomp_settings_set_text("gamepad", key, input_source_name(n));
                        commit();
                    }
                ImGui::EndCombo();
            }
            ImGui::TableNextColumn();
            if (detecting) {
                if (ImGui::Button("Cancel")) capture_cancel();
            } else {
                if (ImGui::Button("Detect")) capture_start(Capture::PAD, "gamepad", key, false);
                ImGui::SameLine();
                if (ImGui::Button("Default")) { recomp_settings_reset_to_default("gamepad", key); commit(); }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static void page_keyboard()
{
    ImGui::TextWrapped("The keys and mouse buttons that press each Xbox control. Each control takes up to four. Press Add, then a key, "
                       "a side or middle mouse button or a wheel notch (Esc cancels); the left and right buttons have buttons of "
                       "their own while you are adding. F11 and Alt+Enter are full screen and cannot be bound.");
    if (ImGui::Button("Reset all to default")) {
        for (int i = 0; i < INPUT_CONTROL_COUNT; i++) recomp_settings_reset_to_default("keyboard", input_control_name(i));
        commit();
    }
    ImGui::SameLine();
    RecompSetting *kp = find("input", "keyboard_player");
    if (kp) {
        ImGui::SameLine();
        ImGui::TextDisabled("Keyboard and mouse play as: %s", kp->choices[kp->stored]);
    }
    ImGui::Spacing();
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Xbox control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthStretch, 1.8f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (int i = 0; i < INPUT_CONTROL_COUNT; i++) {
            const char *key = input_control_name(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(k_control_labels[i]);
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            bool capturing = g_capture.kind == Capture::KEY && g_capture.key == key;
            if (capturing)
                ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "Press a key, side button or wheel...");
            else {
                std::string t = get_text("keyboard", key);
                ImGui::TextUnformatted(t.empty() ? "none" : t.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            if (capturing) {
                if (ImGui::Button("Left click")) capture_commit_key(VK_LBUTTON);
                ImGui::SameLine();
                if (ImGui::Button("Right click")) capture_commit_key(VK_RBUTTON);
                ImGui::SameLine();
                if (ImGui::Button("Cancel")) capture_cancel();
            } else {
                if (ImGui::Button("Add")) capture_start(Capture::KEY, "keyboard", key, true);
                ImGui::SameLine();
                if (ImGui::Button("Clear")) { recomp_settings_set_text("keyboard", key, "none"); commit(); }
                ImGui::SameLine();
                if (ImGui::Button("Default")) { recomp_settings_reset_to_default("keyboard", key); commit(); }
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

static const char *k_show_file_label = "Show settings file";
static void open_folder()
{
    pcui::open_path(pcui::folder_path(pcui::Folder::Settings), true);
}
static void page_general(UiMode mode)
{
    ImGui::SeparatorText("Start-up");
    settings_group("launcher", { "skip" });
    if (mode == UI_OVERLAY)
        ImGui::TextWrapped("With the launcher skipped, hold Shift while the game starts, or start it with --launcher, to see it again.");
    ImGui::SeparatorText("In-game overlay");
    {
        RecompSetting *k = find("ui", "overlay_key");
        if (k) {
            ImGui::AlignTextToFramePadding();
            ImGui::Text("%s:", friendly("ui", "overlay_key"));
            ImGui::SameLine();
            bool capturing = g_capture.kind == Capture::KEY && g_capture.key == "overlay_key";
            if (capturing) {
                ImGui::TextColored(ImVec4(1, 0.75f, 0.2f, 1), "Press a key...");
            } else {
                ImGui::TextUnformatted(k->stored_text);
                ImGui::SameLine();
                if (ImGui::Button("Change##overlaykey")) capture_start(Capture::KEY, "ui", "overlay_key", false);
            }
        }
        RecompSetting *p = find("ui", "overlay_pad");
        if (p) setting_widget(p);
        ImGui::TextWrapped("The overlay opens and closes with the key above or the pad buttons. A combination joined with + (for "
                           "example left_stick_click+right_stick_click) is held for half a second. While it is open the game "
                           "sees no input and keeps running.");
    }
    ImGui::SeparatorText("Appearance");
    settings_group("ui", { "scale" });
    ImGui::SeparatorText("Startup movies");
    settings_group("debug", {"skip_intro"});
    ImGui::TextWrapped("Start at Press Start. Story cutscenes still play. Applies on the next launch.");
}
static void page_maintenance(UiMode mode)
{
    const char *names[] = {"Data", "Saves", "Mods", "Installation", "Settings file", "Play logs"};
    pcui::Folder paths[] = {pcui::Folder::Data,    pcui::Folder::Saves,    pcui::Folder::Mods,
                            pcui::Folder::Install, pcui::Folder::Settings, pcui::Folder::Logs};
    for (int i = 0; i < 6; i++)
    {
        ImGui::PushID(i);
        auto path = pcui::folder_path(paths[i]);
        ImGui::SeparatorText(names[i]);
        ImGui::TextWrapped("%s", path.empty() ? "Not available for this run" : path.c_str());
        if (ImGui::Button("Open"))
            pcui::open_path(path, i == 4);
        ImGui::SameLine();
        if (ImGui::Button("Copy path"))
            pcui::copy_text(path);
        ui_target((std::string("Copy ") + names[i]).c_str());
        ImGui::PopID();
    }
    ImGui::SeparatorText("Update / Repair");
    if (mode == UI_OVERLAY)
        ImGui::TextWrapped("Close the game and open Update / Repair in the launcher to continue in Setup.");
    else
    {
        if (ImGui::Button("Choose Setup application"))
            pcui::setup_choose();
    }
    ImGui::TextWrapped("Setup: %s", pcui::setup_path().empty() ? "not selected" : pcui::setup_path().c_str());
    ImGui::TextWrapped("Use Update / Repair on the launcher's Home screen to open the wizard and close the "
                       "game. Setup verifies releases and builds your dump locally.");
    ImGui::SeparatorText("Files");
    ImGui::TextWrapped("Settings are saved as you change them in: %s", pc_settings_path());
    if (ImGui::Button(k_show_file_label)) open_folder();
    ImGui::SameLine();
    static bool confirm_reset;
    if (ImGui::Button("Reset every setting to default")) confirm_reset = true;
    if (confirm_reset) ImGui::OpenPopup("Reset every setting?");
    if (ImGui::BeginPopupModal("Reset every setting?", &confirm_reset, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Display, controller, key and launcher settings all go back to their defaults.");
        if (ImGui::Button("Reset")) {
            for (size_t i = 0; i < recomp_settings_count(); i++) {
                RecompSetting *s = recomp_settings_at(i);
                recomp_settings_reset_to_default(s->section, s->key);
            }
            commit();
            confirm_reset = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { confirm_reset = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::SeparatorText("About");
    ImGui::Text("Def Jam: Fight for NY - Recompiled, version %s", DEFJAM_VERSION);
    ImGui::TextDisabled("Dear ImGui %s (MIT)  -  SDL3 (zlib)", IMGUI_VERSION);
}

static void page_debug(UiMode mode)
{
    ImGui::SeparatorText("Developer tools");
    settings_group("debug", {"unlock_all"});
    ImGui::TextWrapped("For testing: unlocks fighters and venues and can save the change to your profile. "
                       "Keep off for a save you care about.");
    ImGui::SeparatorText("Texture authoring");
    settings_group("textures", {"dump", "dump_limit"});
    ImGui::TextWrapped("Source captures go to hd-work/runtime. Turn capture off when finished.");
    ImGui::SeparatorText("Play-test recorder");
    settings_group("debug", { "record" });
    ImGui::TextWrapped("Keeps logs/sessions/<time>/ for every run: each frame's timing, what the game was doing, the sound, "
                       "and the last seconds of pictures. The mark key, the mark pad buttons (View and the left stick "
                       "click held together, by default) or the button below save a bug bundle of the "
                       "seconds before it; a crash or a hang saves one by itself. Bundles hold the game's own pictures "
                       "and sound: keep them on this computer. Read one with scripts/bug-bundle.py.");
    ImGui::Indent();
    settings_group("debug", { "mark_key", "mark_pad", "record_fps", "record_seconds", "record_audio", "record_game_state",
                              "auto_bundle_hitch_ms", "hang_seconds", "keep_sessions", "record_guest_ram" });
    ImGui::Unindent();
    if (mode == UI_OVERLAY) {
        char status[1024];
        recorder_status(status, sizeof status);
        ImGui::SeparatorText("This run");
        ImGui::TextWrapped("%s", status);
        if (recorder_on()) {
            if (ImGui::Button("Mark a bug here")) recorder_mark("overlay");
            item_tooltip("Saves the last seconds of play, from before the overlay was opened, as a bug bundle.");
        } else {
            ImGui::TextDisabled("Turn recording on above and restart the game to record.");
        }

        /* Story scenes through the game's developer module (src/hooks/dev_module.c, scene_launcher.c). */
        ImGui::SeparatorText("Story scenes");
        static int pick = 0;
        int count = scene_launcher_count();
        if (count <= 0) {
            ImGui::TextDisabled("The game's scene list is not readable yet.");
            return;
        }
        if (pick >= count) pick = 0;
        char current[16];
        scene_launcher_name(pick, current, sizeof current);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8);
        if (ImGui::BeginCombo("Scene", current)) {
            for (int i = 0; i < count; i++) {
                char name[16];
                scene_launcher_name(i, name, sizeof name);
                if (ImGui::Selectable(name, i == pick)) pick = i;
                if (i == pick) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        int playing = scene_launcher_playing();
        bool active = dev_module_active() != 0;
        ImGui::BeginDisabled(playing >= 0);
        ImGui::SameLine();
        if (ImGui::Button("Play scene")) {
            scene_launcher_request(pick);
            dev_module_request(active ? 0 : 1);
        }
        item_tooltip("Plays the scene in the game's developer cinema mode, then returns to the main menu. "
                              "Whatever is running now (a fight, the Story) is left. Close the overlay to watch.");
        ImGui::EndDisabled();
        if (!active) {
            if (ImGui::Button("Developer menu")) dev_module_request(0);
            item_tooltip("EA's own debug menu, driven by pad 1 (try the d-pad and A). "
                                  "Leaves what is running now.");
        } else if (ImGui::Button("Back to the main menu")) {
            dev_module_leave();
        }
        if (playing >= 0) {
            char name[16];
            scene_launcher_name(playing, name, sizeof name);
            ImGui::Text("Playing %s.", name);
        } else {
            ImGui::TextDisabled(active ? "The developer module is running." : "The developer module is not running.");
        }
    }
}

static int g_page = 0;
static bool g_advanced = false, g_select_page = false, g_return_focus = false, g_setup_request = false;
static std::string g_home_return = "Advanced";
static const char *page_names[] = {"Display", "Controls", "Mods", "Interface", "Diagnostics", "Maintenance"};
static void controls_page()
{
    if (ImGui::CollapsingHeader("Players, devices and feel", ImGuiTreeNodeFlags_DefaultOpen))
        page_controllers();
    if (ImGui::CollapsingHeader("Gamepad button mapping"))
        page_pad_buttons();
    if (ImGui::CollapsingHeader("Keyboard and mouse bindings"))
        page_keyboard();
}
UiResult ui_draw(UiMode mode, float x, float y, float width, float height)
{
    UiResult result;
    g_mode = mode;
    capture_poll_pad();
    g_targets.clear();
    g_help.clear();
    pcui::setup_poll();
    float scale = ImGui::GetStyle().FontScaleMain;
    ImGui::SetNextWindowPos(ImVec2(x, y));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar |
                 ImGuiWindowFlags_NoScrollWithMouse;
    // At maximum text size, keep enough height for the independently scrolling controls.
    bool tight = height < 250 * scale;
    ImVec2 padding = ImGui::GetStyle().WindowPadding;
    if (tight)
        padding.y = std::min(padding.y, 8 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
    ImGui::Begin("##settings", nullptr, flags);
    ImGui::PopStyleVar();
    const char *brand = tight ? "DEF JAM" : "DEF JAM RECOMPILED";
    float header_width = ImGui::CalcTextSize(brand).x +
                         ImGui::CalcTextSize("/ v" DEFJAM_VERSION).x + ImGui::GetStyle().ItemSpacing.x;
    bool version_inline = header_width < ImGui::GetContentRegionAvail().x;
    ImGui::TextUnformatted(brand);
    if (version_inline)
        ImGui::SameLine();
    ImGui::TextDisabled("/ v%s", DEFJAM_VERSION);
    ImGui::Separator();
    bool advanced = mode == UI_OVERLAY || g_advanced;
    bool compact_footer = height < 600 * scale && pcui::action_status().empty();
    float footer = ImGui::GetFrameHeightWithSpacing() * (compact_footer ? 1.0f : 2.1f) +
                   (compact_footer && tight ? 8 : 25) * scale;
    ImGui::BeginChild("page", ImVec2(0, -footer), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    if (!advanced)
    {
        auto home = ui_launcher_home(ImGui::GetContentRegionAvail().x, ImGui::GetContentRegionAvail().y,
                                     g_return_focus ? g_home_return.c_str() : nullptr);
        g_return_focus = false;
        result.play = home.play;
        if (home.route >= 1 && home.route <= 3)
        {
            g_advanced = true;
            ImGui::SetScrollY(0);
            g_page = home.route == 2 ? 1 : home.route == 3 ? 2 : g_page;
            g_select_page = true;
            g_home_return = home.route == 2 ? "Configure" : home.route == 3 ? "Mods" : "Advanced";
        }
        if (home.route == 4)
            g_setup_request = true;
    }
    else
    {
        bool compact_header = height < 500 * scale;
        if (mode == UI_LAUNCHER)
        {
            if (ImGui::Button(compact_header ? "< Back" : "< Back to launcher"))
            {
                g_advanced = false;
                ImGui::SetScrollY(0);
                g_return_focus = true;
            }
            ui_target("Back");
            if (compact_header || width > 650 * scale)
                ImGui::SameLine();
        }
        if (!compact_header)
            pcui::heading("ADVANCED SETTINGS", 24 * scale);
        bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        if (!ui_capturing() && !popup)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false))
            {
                g_page = (g_page + 1) % 6;
                g_select_page = true;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false))
            {
                g_page = (g_page + 5) % 6;
                g_select_page = true;
            }
            if (mode == UI_LAUNCHER && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
            {
                g_advanced = false;
                ImGui::SetScrollY(0);
                g_return_focus = true;
            }
        }
        // At large text sizes a vertical selector keeps every category available.
        bool narrow = ImGui::GetContentRegionAvail().x < 680 * scale;
        if (narrow)
        {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##category", page_names[g_page]))
            {
                for (int i = 0; i < 6; i++)
                    if (ImGui::Selectable(page_names[i], g_page == i))
                        g_page = i;
                ImGui::EndCombo();
            }
            ui_target("Category");
        }
        else if (ImGui::BeginTabBar("categories", ImGuiTabBarFlags_NoCloseWithMiddleMouseButton))
        {
            int requested = g_page;
            for (int i = 0; i < 6; i++)
            {
                bool active =
                    ImGui::BeginTabItem(page_names[i], nullptr,
                                        g_select_page && requested == i ? ImGuiTabItemFlags_SetSelected : 0);
                ui_target(page_names[i]);
                if (active)
                {
                    g_page = i;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        g_select_page = false;
        bool inspector = ImGui::GetContentRegionAvail().x >= 900 * scale;
        ImGui::BeginChild("settings content",
                          ImVec2(inspector ? ImGui::GetContentRegionAvail().x - 280 * scale : 0, 0),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        switch (g_page)
        {
        case 0:
            page_display();
            break;
        case 1:
            controls_page();
            break;
        case 2:
            page_textures();
            break;
        case 3:
            page_general(mode);
            break;
        case 4:
            page_debug(mode);
            break;
        case 5:
            page_maintenance(mode);
            break;
        }
        ImGui::EndChild();
        if (inspector)
        {
            ImGui::SameLine();
            ImGui::BeginChild("context help", ImVec2(0, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            pcui::heading("ABOUT THIS OPTION", 18 * scale);
            ImGui::Spacing();
            ImGui::TextWrapped(
                "%s", g_help.empty()
                          ? "Focus or hover a setting to see what it does. Changes save as you make them."
                          : g_help.c_str());
            ImGui::Spacing();
            ImGui::TextColored(pcui::gold(), mode == UI_LAUNCHER ? "Startup changes apply on Play."
                                                                 : "Startup changes apply next launch.");
            ImGui::EndChild();
        }
    }
    ImGui::EndChild();
    ImGui::Separator();
    std::string restart = restart_needed();
    if (!compact_footer && !pcui::action_status().empty())
    {
        ImGui::BeginChild("last action", ImVec2(0, ImGui::GetTextLineHeight() * 2), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground);
        ImGui::TextWrapped("%s", pcui::action_status().c_str());
        ImGui::EndChild();
    }
    else if (!compact_footer && !restart.empty())
        ImGui::TextWrapped("%s: %s", mode == UI_LAUNCHER ? "Applies on Play" : "Next launch",
                           restart.c_str());
    else if (!compact_footer)
        ImGui::TextDisabled(mode == UI_LAUNCHER ? "Saved as you change / startup options apply on Play"
                                                : "Saved as you change / startup options apply next launch");
    // Keep actions reachable even at 250% text scale; the body scrolls independently.
    if (mode == UI_LAUNCHER)
    {
        if (ImGui::Button("Quit"))
            result.leave = true;
        ui_target("Quit");
        ImGui::SameLine();
        if (advanced || width < 900 * scale)
        {
            if (pcui::play_button("PLAY", ImVec2(100 * scale, 0),
                                  tight ? ImGui::GetFontSize() / scale : 30.0f))
                result.play = true;
            ui_target("Play");
            ImGui::SameLine();
        }
        if (!compact_footer)
            setting_widget(find("launcher", "skip"), "Skip launcher next time");
        else
        {
            const char *status = mode == UI_LAUNCHER || restart.empty() ? "Saved" : "Next launch";
            if (ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize(status).x)
                ImGui::TextDisabled("%s", status);
        }
    }
    else
    {
        if (ImGui::Button("Close"))
            result.leave = true;
        ImGui::SameLine();
        if (!compact_footer)
            setting_widget(find("launcher", "skip"), "Skip launcher next time");
        else
        {
            const char *status = mode == UI_LAUNCHER || restart.empty() ? "Saved" : "Next launch";
            if (ImGui::GetContentRegionAvail().x >= ImGui::CalcTextSize(status).x)
                ImGui::TextDisabled("%s", status);
        }
    }
    if (g_setup_request)
    {
        ImGui::OpenPopup("Update / Repair");
        g_setup_request = false;
    }
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(0, 0), ImVec2(std::max(200.0f, width - 32 * scale), std::max(160.0f, height - 32 * scale)));
    if (ImGui::BeginPopupModal("Update / Repair", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
    {
        ImGui::PushTextWrapPos(std::min(ImGui::GetFontSize() * 34, std::max(120.0f, width - 100 * scale)));
        ImGui::TextWrapped("Open Def Jam Setup to choose a release, update or repair. The launcher will "
                           "close. No installation starts until you choose it in the wizard.");
        ImGui::TextWrapped("Setup: %s",
                           pcui::setup_path().empty() ? "not selected" : pcui::setup_path().c_str());
        if (ImGui::Button("Choose Setup..."))
            pcui::setup_choose();
        if (width > 650 * scale)
            ImGui::SameLine();
        if (ImGui::Button("Download Setup"))
            pcui::open_path("https://github.com/vyanhursky/DefJamFFNY-recomp/releases/latest");
        ImGui::BeginDisabled(!pcui::setup_ready());
        if (ImGui::Button("Open Setup"))
        {
            result.setup = true;
            result.leave = true;
            ImGui::CloseCurrentPopup();
        }
        ui_target("SetupOpen");
        ImGui::EndDisabled();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::PopTextWrapPos();
        ImGui::EndPopup();
    }
    ImGui::End();
    return result;
}

// ---- the overlay ----------------------------------------------------------------------------------------------------

#if !defined(_WIN32)
extern "C" void host_posix_overlay_changed(int open);      // src/host_posix.c
#endif

namespace {

std::atomic<bool> g_open(false);
std::atomic<bool> g_toggle(false);
bool g_just_opened = false;
bool g_ready = false;               // the context and backends exist (under the lock)
#if defined(_WIN32)
HWND g_window = nullptr;
ID3D11Device *g_device = nullptr;
#else
UiWindow g_window = nullptr;        // unused: the overlay is drawn in the picture's own window
#endif
int g_overlay_vk = VK_F1;

struct Chord { int src[3]; int n; UiTick since; bool fired; };
std::vector<Chord> g_chords;        // read and written on the drawing thread, and by apply_settings under the lock
std::vector<Chord> g_mark_chords;   // [debug] mark_pad: the recorder's mark, from a pad

void parse_chord_list(const std::string &text, std::vector<Chord> &chords)
{
    chords.clear();
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t comma = text.find(',', pos);
        std::string alt = text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = comma == std::string::npos ? text.size() + 1 : comma + 1;
        Chord c = {};
        size_t p = 0;
        while (p <= alt.size() && c.n < 3) {
            size_t plus = alt.find('+', p);
            std::string name = alt.substr(p, plus == std::string::npos ? std::string::npos : plus - p);
            p = plus == std::string::npos ? alt.size() + 1 : plus + 1;
            size_t a = name.find_first_not_of(" \t"), b = name.find_last_not_of(" \t");
            if (a == std::string::npos) continue;
            int src = input_source_from_name(name.substr(a, b - a + 1).c_str());
            if (src > INPUT_SRC_NONE) c.src[c.n++] = src;
        }
        if (c.n) chords.push_back(c);
    }
}

void parse_chords()
{
    parse_chord_list(get_text("ui", "overlay_pad"), g_chords);
    parse_chord_list(get_text("debug", "mark_pad"), g_mark_chords);
}

// A chord of `chords` was just completed, on any pad: one button at once, two or three held half a second.
bool chords_pressed(std::vector<Chord> &chords)
{
    InputRaw all;
    memset(&all, 0, sizeof(all));
    for (int slot = 0; slot < 4; slot++) {
        InputRaw raw;
        if (!xbox_HostInputRawPad(slot, &raw)) continue;
        for (int s = 0; s < INPUT_SRC_COUNT; s++)
            if (raw.src[s] > all.src[s]) all.src[s] = raw.src[s];
    }
    UiTick now = ui_ticks();
    bool fire = false;
    for (Chord &c : chords) {
        bool held = true;
        for (int i = 0; i < c.n; i++) held = held && all.src[c.src[i]] >= 16384;
        if (!held) { c.since = 0; c.fired = false; continue; }
        if (!c.since) c.since = now;
        if (!c.fired && now - c.since >= (c.n > 1 ? 500u : 0u)) { c.fired = true; fire = true; }
    }
    return fire;
}

// Pad buttons that open and close the overlay, from any pad.
bool pad_toggle_pressed() { return chords_pressed(g_chords); }

// The recorder's mark from a pad; checked once a frame wherever the overlay chord is.
void pad_mark_poll()
{
    if (recorder_on() && !g_mark_chords.empty() && !ui_capturing() && chords_pressed(g_mark_chords))
        recorder_mark("pad");
}

void set_open(bool open)
{
    if (g_open.load() == open) return;
    g_open = open;
#if defined(__ANDROID__)
    // Closing by touch can precede the Android UP event; never reopen with a stuck press.
    if (!open && g_ready) ImGui::GetIO().AddMouseButtonEvent(0, false);
#endif
    g_just_opened = open;
    xbox_HostInputSetUiActive(open ? 1 : 0);
#if !defined(_WIN32)
    host_posix_overlay_changed(open ? 1 : 0);       // the pointer shows, and typing is delivered
#endif
    fprintf(stderr, "[UI] overlay %s\n", open ? "opened" : "closed");
    fflush(stderr);
    recorder_event("overlay", open ? "\"open\":true" : "\"open\":false");
}

} // namespace

#if defined(_WIN32)
extern "C" void pc_ui_set_window(HWND window) { g_window = window; }
#endif

extern "C" int pc_ui_overlay_open(void) { return g_open.load() ? 1 : 0; }

extern "C" void pc_ui_apply_settings(void)
{
    ui_lock();
    std::string key = get_text("ui", "overlay_key");
    int vk = input_key_from_name(key.c_str());
    g_overlay_vk = vk > 0 ? vk : VK_F1;
    parse_chords();
    if (g_ready) ui_apply_scale(g_window);
    ui_unlock();
}

#if defined(_WIN32)
extern "C" int pc_ui_window_message(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && recorder_mark_vk() &&
        (int)wp == recorder_mark_vk() && (int)wp != g_overlay_vk) {
        ui_lock();
        bool capturing = ui_capturing();
        ui_unlock();
        if (!capturing) {
            recorder_mark("key");
            return 1;
        }
    }
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && !(lp & (1 << 30)) && (int)wp == g_overlay_vk) {
        ui_lock();
        bool capturing = ui_capturing();
        ui_unlock();
        if (!capturing) {
            g_toggle = true;
            return 1;
        }
    }
    if (!g_open.load()) return 0;
    ui_lock();
    int handled = 0;
    if (g_ready) {
        if (ui_capture_message(window, msg, wp, lp)) handled = 1;
        else ImGui_ImplWin32_WndProcHandler(window, msg, wp, lp);
    }
    ui_unlock();
    return handled;
}

extern "C" void pc_ui_overlay(void *device, void *context, void *window_rtv, unsigned width, unsigned height, void *)
{
    static bool configured;
    if (!configured) {
        configured = true;
        pc_ui_apply_settings();
    }
    ui_lock();
    bool toggled = g_toggle.exchange(false);
    pad_mark_poll();
    if (!ui_capturing() && !g_chords.empty() && pad_toggle_pressed()) toggled = true;
    if (toggled) set_open(!g_open.load());
    if (!g_open.load()) { ui_unlock(); return; }

    ID3D11Device *dev = (ID3D11Device *)device;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)context;
    if (!g_ready) {
        if (!g_window) { ui_unlock(); return; }
        ui_context_begin(g_window);
        ImGui_ImplWin32_Init(g_window);
        ImGui_ImplDX11_Init(dev, ctx);
        g_device = dev;
        g_ready = true;
        fprintf(stderr, "[UI] overlay ready (Dear ImGui %s)\n", IMGUI_VERSION);
        fflush(stderr);
    }

    {
        float h = height / 960.0f;
        g_extra_scale = h < 1.0f ? 1.0f : h > 2.4f ? 2.4f : h;
    }
    ui_apply_scale(g_window);
    ui_feed_gamepad();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    // Dim the game and put the settings in a panel in the middle.
    ImGuiIO &io = ImGui::GetIO();
    if (g_just_opened) {
        g_just_opened = false;
        ImGui::SetNextWindowFocus();
    }
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    float pw = io.DisplaySize.x * 0.82f, ph = io.DisplaySize.y * 0.86f;
    if (pw < 560) pw = io.DisplaySize.x;
    if (ph < 420) ph = io.DisplaySize.y;
    bool close_request = false;
    {
        ImGui::SetNextWindowBgAlpha(0.96f);
        UiResult r = ui_draw(UI_OVERLAY, (io.DisplaySize.x - pw) / 2, (io.DisplaySize.y - ph) / 2, pw, ph);
        close_request = r.leave;
    }
    // Escape or the pad's cancel button closes the overlay, unless a popup or a capture is open.
    if (!ui_capturing() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        close_request = true;

    ImGui::Render();
    ID3D11RenderTargetView *old_rtv = nullptr;
    ID3D11DepthStencilView *old_dsv = nullptr;
    ctx->OMGetRenderTargets(1, &old_rtv, &old_dsv);
    ID3D11RenderTargetView *rtv = (ID3D11RenderTargetView *)window_rtv;
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    ctx->OMSetRenderTargets(1, &old_rtv, old_dsv);       // the title's own target, as it was
    if (old_rtv) old_rtv->Release();
    if (old_dsv) old_dsv->Release();
    (void)width;
    if (close_request) set_open(false);
    ui_unlock();
}
#endif

#if !defined(_WIN32)
// ---- off Windows: SDL events, and the overlay through the Vulkan present --------------------------------------------

static ImGuiKey imgui_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return (ImGuiKey)(ImGuiKey_A + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (ImGuiKey)(ImGuiKey_1 + (sc - SDL_SCANCODE_1));
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return (ImGuiKey)(ImGuiKey_F1 + (sc - SDL_SCANCODE_F1));
    switch (sc) {
    case SDL_SCANCODE_0: return ImGuiKey_0;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_INSERT: return ImGuiKey_Insert;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_RETURN: return ImGuiKey_Enter;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDL_SCANCODE_MINUS: return ImGuiKey_Minus;
    case SDL_SCANCODE_EQUALS: return ImGuiKey_Equal;
    case SDL_SCANCODE_PERIOD: return ImGuiKey_Period;
    case SDL_SCANCODE_COMMA: return ImGuiKey_Comma;
    case SDL_SCANCODE_SLASH: return ImGuiKey_Slash;
    case SDL_SCANCODE_LSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_RSHIFT: return ImGuiKey_RightShift;
    case SDL_SCANCODE_LCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_RCTRL: return ImGuiKey_RightCtrl;
    case SDL_SCANCODE_LALT: return ImGuiKey_LeftAlt;
    case SDL_SCANCODE_RALT: return ImGuiKey_RightAlt;
    case SDL_SCANCODE_LGUI: return ImGuiKey_LeftSuper;
    case SDL_SCANCODE_RGUI: return ImGuiKey_RightSuper;
    default: return ImGuiKey_None;
    }
}

static float g_extra_scale_set = 1.0f;
static unsigned long long g_last_frame_ns;

void ui_set_extra_scale(float scale) { g_extra_scale = scale; (void)g_extra_scale_set; }

bool ui_sdl_event(const SDL_Event *e, float scale_x, float scale_y)
{
    ImGuiIO &io = ImGui::GetIO();
    switch (e->type) {
#if defined(__ANDROID__)
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
        io.ConfigFlags |= ImGuiConfigFlags_IsTouchScreen;
        io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
        io.AddMousePosEvent(e->tfinger.x * io.DisplaySize.x, e->tfinger.y * io.DisplaySize.y);
        if (e->type == SDL_EVENT_FINGER_DOWN) io.AddMouseButtonEvent(0, true);
        else if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED)
            io.AddMouseButtonEvent(0, false);
        return true;
#endif
    case SDL_EVENT_MOUSE_MOTION:
        io.AddMousePosEvent(e->motion.x * scale_x, e->motion.y * scale_y);
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1 :
                e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        io.AddMousePosEvent(e->button.x * scale_x, e->button.y * scale_y);
        if (b >= 0) io.AddMouseButtonEvent(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(e->wheel.x, e->wheel.y);
        return true;
    case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
        return true;
    case SDL_EVENT_TEXT_INPUT:
        io.AddInputCharactersUTF8(e->text.text);
        return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        SDL_Keymod mod = e->key.mod;
        io.AddKeyEvent(ImGuiMod_Ctrl, (mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (mod & SDL_KMOD_ALT) != 0);
        io.AddKeyEvent(ImGuiMod_Super, (mod & SDL_KMOD_GUI) != 0);
        ImGuiKey k = imgui_key(e->key.scancode);
        if (k != ImGuiKey_None) io.AddKeyEvent(k, e->type == SDL_EVENT_KEY_DOWN);
        return true;
    }
    default:
        return false;
    }
}

void ui_sdl_frame(float width, float height, float framebuffer_scale)
{
    ImGuiIO &io = ImGui::GetIO();
    unsigned long long now = SDL_GetTicksNS();
    io.DisplaySize = ImVec2(width, height);
    io.DisplayFramebufferScale = ImVec2(framebuffer_scale, framebuffer_scale);
    io.DeltaTime = g_last_frame_ns ? (float)((now - g_last_frame_ns) / 1e9) : 1.0f / 60.0f;
    if (io.DeltaTime < 0.0001f) io.DeltaTime = 0.0001f;
    if (io.DeltaTime > 0.25f) io.DeltaTime = 0.25f;
    g_last_frame_ns = now;
}

// ---- the overlay, drawn by the toolkit's Vulkan present --------------------------------------------------------------

#include "d3d8_vk.h"

namespace {

uint32_t g_vk_generation;
std::atomic<unsigned> g_frame_w(0), g_frame_h(0);       // what the overlay was last drawn into, in pixels

// Asked once a frame whether to draw: this is where the hotkey and the pad chord are looked for.
int overlay_active(void *)
{
    static bool configured;
    if (!configured) {
        configured = true;
        pc_ui_apply_settings();
    }
    ui_lock();
    bool toggled = g_toggle.exchange(false);
    pad_mark_poll();
    if (!ui_capturing() && !g_chords.empty() && pad_toggle_pressed()) toggled = true;
    if (toggled) set_open(!g_open.load());
    bool open = g_open.load();
    ui_unlock();
    return open ? 1 : 0;
}

void overlay_init_backend(const D3D8VkOverlayFrame *f)
{
    ImGui_ImplVulkan_InitInfo info = {};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = (VkInstance)f->instance;
    info.PhysicalDevice = (VkPhysicalDevice)f->physical_device;
    info.Device = (VkDevice)f->device;
    info.QueueFamily = f->queue_family;
    info.Queue = (VkQueue)f->queue;
    info.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE;
    info.MinImageCount = 2;
    info.ImageCount = f->image_count < 2 ? 2 : f->image_count;
    info.PipelineInfoMain.RenderPass = (VkRenderPass)f->render_pass;
    ImGui_ImplVulkan_Init(&info);
    g_vk_generation = f->generation;
}

void overlay_draw(const D3D8VkOverlayFrame *f, void *)
{
    ui_lock();
    if (!g_open.load()) { ui_unlock(); return; }
    if (g_ready && f->generation != g_vk_generation) {
        // The window's image has another format: the pipelines were built for the old pass.
        ImGui_ImplVulkan_Shutdown();
        overlay_init_backend(f);
    }
    if (!g_ready) {
        ui_context_begin(nullptr);
        g_last_frame_ns = 0;
        overlay_init_backend(f);
        g_ready = true;
        fprintf(stderr, "[UI] overlay ready (Dear ImGui %s, Vulkan)\n", IMGUI_VERSION);
        fflush(stderr);
    }

    g_frame_w = f->width;
    g_frame_h = f->height;
    {
        float h = f->height / 960.0f;
        ui_set_extra_scale(h < 1.0f ? 1.0f : h > 2.4f ? 2.4f : h);
    }
    ui_apply_scale(nullptr);
    ui_feed_gamepad();
    ImGui_ImplVulkan_NewFrame();
    ui_sdl_frame((float)f->width, (float)f->height, 1.0f);
    ImGui::NewFrame();

    // Dim the game and put the settings in a panel in the middle (as the Direct3D overlay does).
    ImGuiIO &io = ImGui::GetIO();
    if (g_just_opened) {
        g_just_opened = false;
        ImGui::SetNextWindowFocus();
    }
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    bg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    float pw = io.DisplaySize.x * 0.82f, ph = io.DisplaySize.y * 0.86f;
    if (pw < 560) pw = io.DisplaySize.x;
    if (ph < 420) ph = io.DisplaySize.y;
    bool close_request = false;
    {
        ImGui::SetNextWindowBgAlpha(0.96f);
        UiResult r = ui_draw(UI_OVERLAY, (io.DisplaySize.x - pw) / 2, (io.DisplaySize.y - ph) / 2, pw, ph);
        close_request = r.leave;
    }
    // Escape or the pad's cancel button closes the overlay, unless a popup or a capture is open.
    if (!ui_capturing() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)))
        close_request = true;

    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), (VkCommandBuffer)f->command_buffer);
    if (close_request) set_open(false);
    ui_unlock();
}

} // namespace

// Offer an SDL event to the overlay, on the thread that reads the window's events. Returns non-zero
// when the overlay used it (its key, a key being captured for a binding) and it should go no further.
extern "C" int pc_ui_sdl_event(const void *event, float window_width, float window_height)
{
    const SDL_Event *e = (const SDL_Event *)event;
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat && recorder_mark_vk() &&
        host_posix_vk_from_scancode(e->key.scancode) == recorder_mark_vk() &&
        host_posix_vk_from_scancode(e->key.scancode) != g_overlay_vk) {
        ui_lock();
        bool capturing = ui_capturing();
        ui_unlock();
        if (!capturing) {
            recorder_mark("key");
            return 1;
        }
    }
    if (e->type == SDL_EVENT_KEY_DOWN && !e->key.repeat &&
        host_posix_vk_from_scancode(e->key.scancode) == g_overlay_vk) {
        ui_lock();
        bool capturing = ui_capturing();
        ui_unlock();
        if (!capturing) {
            g_toggle = true;
            return 1;
        }
    }
    if (!g_open.load()) return 0;
    ui_lock();
    int handled = 0;
    if (g_ready) {
        if (ui_capture_sdl_event(e)) {
            handled = 1;
        } else {
            // The pointer is in the window's points; the overlay is drawn in the pixels of the
            // image it was last drawn into, which need not be the window's own pixel size.
            float fw = (float)g_frame_w.load(), fh = (float)g_frame_h.load();
            float sx = window_width > 0 && fw > 0 ? fw / window_width : 1.0f;
            float sy = window_height > 0 && fh > 0 ? fh / window_height : 1.0f;
            handled = ui_sdl_event(e, sx, sy) ? 1 : 0;
        }
    }
    ui_unlock();
    return handled;
}

// Hand the overlay to the Vulkan present step. Call before the graphics device is made.
extern "C" void pc_ui_register_overlay(void)
{
    D3D8VkOverlay overlay = { overlay_active, overlay_draw, nullptr };
    d3d8_vk_set_overlay(&overlay);
}
#endif
