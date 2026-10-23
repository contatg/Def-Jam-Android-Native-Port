/*
 * The port's input: settings, start-up and the window's keys and mouse.
 *
 * The generic work -- reading SDL and XInput pads, the keyboard as a player,
 * remapping, deadzones, rumble -- is the toolkit's (src/input/input_host.c and
 * input_map.c). This file is the game's side of it:
 *
 *   settings.ini   [input] who plays what, rumble and the cursor; [gamepad] the
 *                  pad map and deadzones; [keyboard] one line of keys per Xbox
 *                  control. All of it goes through the settings library, so the
 *                  overlay and launcher later edit the same values.
 *   start-up       pc_input_start opens the pads and sizes the emulated hub
 *                  (RECOMP_USB_PADS) to the controllers the title should see.
 *   the window     keys, mouse buttons, the wheel and focus changes arrive as
 *                  window messages on the window's thread.
 *
 * Scripted runs and RECOMP_SETTINGS=none leave all of this off: the harness
 * drives pad 1 from a script and must not gain a keyboard player or a pad of
 * the developer's that happens to be plugged in.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../host.h"   /* by its place, so the file also builds on its own (tests/unit) */
#include "input_host.h"
#include "input_map.h"
#include "pc_input.h"
#include "recomp_settings.h"

/* ---- the settings table ----------------------------------------------------- */

static const char *const k_keyboard_player[] = { "auto", "off", "merge", "1", "2", "3", "4", NULL };
static const char *const k_backend[] = { "sdl", "xinput", NULL };
static const char *const k_stick[] = { "left", "right", "none", NULL };

/* The keys each Xbox control starts with. The face buttons sit under the right
 * hand in the shape of the pad's diamond (I top, J left, L right, K bottom);
 * the left hand has the left stick, the triggers and run. The arrow keys are
 * the right stick, which the title uses for Blazin' moves. */
static const char *const k_default_keys[INPUT_CONTROL_COUNT] = {
    /* dpad up/down/left/right */ "Numpad8", "Numpad2", "Numpad4", "Numpad6",
    /* start, back */             "Enter", "Backspace",
    /* left thumb, right thumb */ "LCtrl", "RCtrl",
    /* A B X Y */                 "K, Space, Mouse1", "L, LShift, Mouse2", "J", "I",
    /* black, white */            "U", "O",
    /* left, right trigger */     "Q, Mouse4", "E, Mouse5",
    /* left stick */              "W", "S", "A", "D",
    /* right stick */             "Up", "Down", "Left", "Right",
};

static const char *const k_control_help[INPUT_CONTROL_COUNT] = {
    "Keys for each Xbox control, separated by commas: letters and digits, F1-F12, Up Down Left Right, Space, Enter, "
    "Backspace, Tab, Escape, Shift/LShift/RShift, Ctrl/LCtrl/RCtrl, Alt, Numpad0-Numpad9, Mouse1-Mouse5, WheelUp, "
    "WheelDown, or none. Up to four per control. F11 and Alt+Enter are full screen. This is the keyboard player's pad.",
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL,
};

static const char *const k_source_help =
    "Which physical control acts as each Xbox control. Names are positional: south is the bottom face button (Xbox A, "
    "PlayStation cross), east the right one, west the left, north the top. Also back, start, guide, "
    "left_stick_click, right_stick_click, left_shoulder, right_shoulder, dpad_up/down/left/right, left_trigger, "
    "right_trigger, misc1, touchpad, paddle1-4, left_stick_up/down/left/right and right_stick_up/down/left/right, or none. "
    "Applies to every pad.";

size_t pc_input_settings(RecompSetting *out, size_t max)
{
    InputPadMap def;
    size_t n = 0;
    int i;

    input_padmap_defaults(&def);
    if (max < 64) return 0;
    memset(out, 0, sizeof(*out) * 64);

#define ADD(...) do { RecompSetting s_ = { __VA_ARGS__ }; out[n++] = s_; } while (0)
    ADD("input", "keyboard_player", RECOMP_SETTING_ENUM, 0, 0, 0, k_keyboard_player, "RECOMP_KEYBOARD_PLAYER",
        RECOMP_SETTING_RESTART,
        "How the keyboard and mouse play: auto makes them their own player after the pads (player 1 when no pad "
        "is connected); off; merge adds them to player 1's pad; 1 to 4 picks the player.");
    ADD("input", "players", RECOMP_SETTING_INT, 0, 0, 4, NULL, NULL, RECOMP_SETTING_RESTART,
        "Controllers the game sees. 0 is one per pad plus one for the keyboard. Raise it to hot-plug pads later.");
    ADD("input", "backend", RECOMP_SETTING_ENUM, 0, 0, 0, k_backend, "RECOMP_INPUT_BACKEND", RECOMP_SETTING_RESTART,
        "Where pads are read: sdl (DualSense, Switch Pro, Xbox and most others) or xinput (Xbox-style pads only).");
    ADD("input", "rumble", RECOMP_SETTING_INT, 100, 0, 100, NULL, NULL, 0,
        "Rumble strength in percent; 0 turns it off.");
    ADD("input", "rumble_floor", RECOMP_SETTING_INT, 60, 0, 100, NULL, NULL, 0,
        "Weakest rumble pulse in percent of full strength: the game's pulses are often faint and brief, and a pulse "
        "weaker than this is raised to it. 0 passes the game's strength through unchanged.");
    ADD("input", "rumble_min_ms", RECOMP_SETTING_INT, 200, 0, 1000, NULL, NULL, 0,
        "Shortest rumble pulse in milliseconds: the game's pulses are 50-200 ms, which many pads do not make "
        "felt. 0 passes the game's timing through unchanged.");
    ADD("input", "rumble_on_connect", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, NULL, 0,
        "Buzz a pad for a moment when it takes a player, so you can tell it works and which one it is.");
    ADD("input", "hide_cursor", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, NULL, 0,
        "Hide the mouse pointer over the game when it has not moved for two seconds.");
    ADD("input", "confine_cursor", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, NULL, 0,
        "Keep the mouse inside the window in full screen, so a click cannot land on another monitor.");

    ADD("gamepad", "left_deadzone", RECOMP_SETTING_INT, def.deadzone_left, 0, 90, NULL, NULL, 0,
        "Deadzone of the left stick in percent of its travel.");
    ADD("gamepad", "right_deadzone", RECOMP_SETTING_INT, def.deadzone_right, 0, 90, NULL, NULL, 0,
        "Deadzone of the right stick in percent.");
    ADD("gamepad", "trigger_threshold", RECOMP_SETTING_INT, def.trigger_threshold, 0, 90, NULL, NULL, 0,
        "How far a trigger must be pulled before it counts, in percent.");
    ADD("gamepad", "button_threshold", RECOMP_SETTING_INT, def.button_threshold, 10, 90, NULL, NULL, 0,
        "How far a trigger or stick must travel to count as a button, when one is mapped to a button.");
    ADD("gamepad", "left_stick", RECOMP_SETTING_ENUM, def.left_stick, 0, 0, k_stick, NULL, 0,
        "Which physical stick is the Xbox left stick.");
    ADD("gamepad", "right_stick", RECOMP_SETTING_ENUM, def.right_stick, 0, 0, k_stick, NULL, 0,
        "Which physical stick is the Xbox right stick (the title uses it for Blazin' moves).");
    for (i = 0; i < INPUT_PAD_CONTROL_COUNT; i++)
        ADD("gamepad", input_control_name(i), RECOMP_SETTING_STRING, 0, 0, 0, NULL, NULL, 0,
            i == 0 ? k_source_help : NULL, input_source_name(def.source[i]));

    for (i = 0; i < INPUT_CONTROL_COUNT; i++)
        ADD("keyboard", input_control_name(i), RECOMP_SETTING_STRING, 0, 0, 0, NULL, NULL, 0,
            k_control_help[i], k_default_keys[i]);
#undef ADD
    if (n > 64) { fprintf(stderr, "[INPUT] settings table overflow (%u)\n", (unsigned)n); abort(); }
    return n;
}

/* ---- the running layer --------------------------------------------------------- */

static int g_started;
static int g_usb_env_ours;      /* RECOMP_USB_PADS was set here, not by the caller */

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return v && *v && *v != '0';
}

static int geti(const char *section, const char *key, int fallback)
{
    return recomp_settings_get(section, key, fallback);
}

/* The settings as the toolkit's configuration. */
static void build_config(InputHostConfig *cfg)
{
    static const int modes[] = {
        INPUT_HOST_KEYBOARD_AUTO, INPUT_HOST_KEYBOARD_OFF, INPUT_HOST_KEYBOARD_MERGE,
        INPUT_HOST_KEYBOARD_SLOT1, INPUT_HOST_KEYBOARD_SLOT2, INPUT_HOST_KEYBOARD_SLOT3,
        INPUT_HOST_KEYBOARD_SLOT4 };
    int mode = geti("input", "keyboard_player", 0), i;
    char text[RECOMP_SETTING_TEXT_MAX];

    xbox_HostInputDefaults(cfg);
    cfg->keyboard = modes[mode >= 0 && mode < 7 ? mode : 0];
    cfg->players = geti("input", "players", 0);
    cfg->use_sdl = geti("input", "backend", 0) == 0;
    cfg->rumble_percent = geti("input", "rumble", 100);
    cfg->rumble_floor = geti("input", "rumble_floor", 60);
    cfg->rumble_min_ms = geti("input", "rumble_min_ms", 200);
    cfg->rumble_on_connect = geti("input", "rumble_on_connect", 1);
    /* Diagnostics for unattended runs: no physical pad, and no focus needed. */
    cfg->no_pads = env_on("RECOMP_INPUT_NO_PADS");
    cfg->virtual_pads = getenv("RECOMP_INPUT_VIRTUAL_PADS") ? atoi(getenv("RECOMP_INPUT_VIRTUAL_PADS")) : 0;
    cfg->virtual_late_ms = getenv("RECOMP_INPUT_VIRTUAL_LATE_MS") ? atoi(getenv("RECOMP_INPUT_VIRTUAL_LATE_MS")) : 0;
    cfg->virtual_chord_ms = getenv("RECOMP_INPUT_VIRTUAL_CHORD_MS") ? atoi(getenv("RECOMP_INPUT_VIRTUAL_CHORD_MS")) : 0;
    cfg->ignore_focus = env_on("RECOMP_INPUT_IGNORE_FOCUS");

    cfg->padmap.deadzone_left = (uint8_t)geti("gamepad", "left_deadzone", 15);
    cfg->padmap.deadzone_right = (uint8_t)geti("gamepad", "right_deadzone", 15);
    cfg->padmap.trigger_threshold = (uint8_t)geti("gamepad", "trigger_threshold", 5);
    cfg->padmap.button_threshold = (uint8_t)geti("gamepad", "button_threshold", 50);
    cfg->padmap.left_stick = (uint8_t)geti("gamepad", "left_stick", INPUT_STICK_LEFT);
    cfg->padmap.right_stick = (uint8_t)geti("gamepad", "right_stick", INPUT_STICK_RIGHT);
    for (i = 0; i < INPUT_PAD_CONTROL_COUNT; i++) {
        int source;
        recomp_settings_get_text("gamepad", input_control_name(i), text, sizeof(text), "");
        source = input_source_from_name(text);
        if (source < 0)
            fprintf(stderr, "[INPUT] [gamepad] %s = %s is not a pad control; the default stays\n",
                    input_control_name(i), text);
        else
            cfg->padmap.source[i] = (uint8_t)source;
    }

    for (i = 0; i < INPUT_CONTROL_COUNT; i++) {
        int rejected;
        recomp_settings_get_text("keyboard", input_control_name(i), text, sizeof(text), "");
        rejected = input_bindings_parse(&cfg->keys, i, text);
        if (rejected)
            fprintf(stderr, "[INPUT] [keyboard] %s: %d name(s) are not keys and were ignored\n",
                    input_control_name(i), rejected);
    }
}

/* The hub is built with the controllers present at start. A pad plugged in
 * later takes a free player; this plugs one more controller into the hub for it,
 * which the title sees as an ordinary hot-plug. */
void xbox_UsbSetPadCount(int n);

void pc_input_start(void)
{
    InputHostConfig cfg;
    const char *settings = getenv("RECOMP_SETTINGS");
    const char *host = getenv("RECOMP_INPUT_HOST");
    const char *usb_pads = getenv("RECOMP_USB_PADS");
    int test_mode = settings && !_stricmp(settings, "none");
    int scripted = getenv("RECOMP_PAD_SCRIPT") && !env_on("RECOMP_PAD_HOST");
    int found;
    char count[8];

    if (host && *host == '0') {
        fprintf(stderr, "[INPUT] host input off (RECOMP_INPUT_HOST=0); the toolkit's XInput path answers\n");
        return;
    }
    if ((test_mode || scripted) && !env_on("RECOMP_INPUT_HOST")) {
        fprintf(stderr, "[INPUT] host input off for %s runs\n", scripted ? "scripted" : "RECOMP_SETTINGS=none");
        return;
    }
    build_config(&cfg);
    if (usb_pads && atoi(usb_pads) > cfg.players)
        cfg.players = atoi(usb_pads);       /* the caller asked for at least this many */
    xbox_HostInputOnPadSlot(xbox_UsbSetPadCount);
    found = xbox_HostInputStart(&cfg);
    g_started = 1;
    atexit(pc_input_stop);
    snprintf(count, sizeof(count), "%d", xbox_HostInputPlayerCount());
    if (!usb_pads || !*usb_pads) {
        _putenv_s("RECOMP_USB_PADS", count);
        g_usb_env_ours = 1;
    }
    fprintf(stderr, "[INPUT] %d pad(s), %s controller(s) on the hub\n", found, count);
    fflush(stderr);
}

void pc_input_stop(void)
{
    if (!g_started) return;
    g_started = 0;
    xbox_HostInputStop();
}

void pc_input_restart(void)
{
    if (!g_started) return;
    pc_input_stop();
    if (g_usb_env_ours) {
        _putenv_s("RECOMP_USB_PADS", "");
        g_usb_env_ours = 0;
    }
    pc_input_start();
}

void pc_input_current_padmap(InputPadMap *map)
{
    InputHostConfig cfg;
    build_config(&cfg);
    *map = cfg.padmap;
}

void pc_input_apply(void)
{
    InputHostConfig cfg;
    if (!g_started) return;
    build_config(&cfg);
    xbox_HostInputConfigure(&cfg);
}

/* ---- the window ----------------------------------------------------------------- */

static int g_buttons_down;      /* bit per mouse button held, for capture */
static int g_wheel_rest;        /* high-resolution wheel deltas not yet a notch */
static uint8_t g_side[2][3];    /* left/right shift, ctrl, alt */

static void key_event(int vk, int down)
{
    xbox_HostInputKey(vk, down);
}

#if defined(_WIN32)

/* Shift, Ctrl and Alt arrive as one virtual key for both sides; the scan code
 * and the extended bit say which. Report the side and the generic key, which
 * is held while either side is. */
static void modifier_event(int generic, int index, int right, int down)
{
    static const int left_vk[3] = { VK_LSHIFT, VK_LCONTROL, VK_LMENU };
    static const int right_vk[3] = { VK_RSHIFT, VK_RCONTROL, VK_RMENU };
    g_side[right][index] = (uint8_t)down;
    key_event(right ? right_vk[index] : left_vk[index], down);
    key_event(generic, g_side[0][index] || g_side[1][index]);
}

int pc_input_vk_from_message(WPARAM wp, LPARAM lp)
{
    int vk = (int)wp;
    if (vk == VK_SHIFT)
        return MapVirtualKeyW((UINT)((lp >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX) == VK_RSHIFT ? VK_RSHIFT : VK_LSHIFT;
    if (vk == VK_CONTROL) return ((lp >> 24) & 1) ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ((lp >> 24) & 1) ? VK_RMENU : VK_LMENU;
    return vk;
}

static void keyboard_message(UINT msg, WPARAM wp, LPARAM lp)
{
    int down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
    int vk = (int)wp;
    if (vk == VK_SHIFT) {
        int scan = (lp >> 16) & 0xFF;
        modifier_event(VK_SHIFT, 0, MapVirtualKeyW((UINT)scan, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT, down);
    } else if (vk == VK_CONTROL) {
        modifier_event(VK_CONTROL, 1, (lp >> 24) & 1, down);
    } else if (vk == VK_MENU) {
        modifier_event(VK_MENU, 2, (lp >> 24) & 1, down);
    } else if (vk == VK_RETURN && down && (g_side[0][2] || g_side[1][2])) {
        /* Alt+Enter is the full screen chord, not the Start button. */
    } else {
        key_event(vk, down);
    }
}

static void mouse_button(HWND window, int vk, int down)
{
    int bit = 1 << (vk & 7);
    if (down) {
        if (!g_buttons_down) SetCapture(window);
        g_buttons_down |= bit;
    } else {
        g_buttons_down &= ~bit;
        if (!g_buttons_down && GetCapture() == window) ReleaseCapture();
    }
    key_event(vk, down);
}

#endif /* _WIN32 */

static void release_everything(void)
{
    int vk;
    g_buttons_down = 0;
    g_wheel_rest = 0;
    memset(g_side, 0, sizeof(g_side));
    for (vk = 0; vk < 256; vk++) xbox_HostInputKey(vk, 0);
}

void pc_input_set_focus(int focused)
{
    xbox_HostInputFocus(focused);
    if (!focused) release_everything();
}

#if defined(_WIN32)

void pc_input_window_message(HWND window, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
        keyboard_message(msg, wp, lp);
        break;
    case WM_LBUTTONDOWN: mouse_button(window, VK_LBUTTON, 1); break;
    case WM_LBUTTONUP:   mouse_button(window, VK_LBUTTON, 0); break;
    case WM_RBUTTONDOWN: mouse_button(window, VK_RBUTTON, 1); break;
    case WM_RBUTTONUP:   mouse_button(window, VK_RBUTTON, 0); break;
    case WM_MBUTTONDOWN: mouse_button(window, VK_MBUTTON, 1); break;
    case WM_MBUTTONUP:   mouse_button(window, VK_MBUTTON, 0); break;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
        int vk = HIWORD(wp) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        mouse_button(window, vk, msg == WM_XBUTTONDOWN);
        break;
    }
    case WM_MOUSEWHEEL: {
        int notches;
        g_wheel_rest += GET_WHEEL_DELTA_WPARAM(wp);
        notches = g_wheel_rest / WHEEL_DELTA;
        if (notches) {
            g_wheel_rest -= notches * WHEEL_DELTA;
            xbox_HostInputWheel(notches);
        }
        break;
    }
    case WM_SETFOCUS:
        pc_input_set_focus(1);
        break;
    case WM_KILLFOCUS:
        pc_input_set_focus(0);
        break;
    case WM_ACTIVATEAPP:
        if (!wp) pc_input_set_focus(0);
        break;
    case WM_CAPTURECHANGED:
        /* Capture taken away (a dialog, Alt-Tab): the buttons are no longer ours to see released. */
        if ((HWND)lp != window && g_buttons_down) {
            int vk;
            g_buttons_down = 0;
            for (vk = 1; vk < 7; vk++) xbox_HostInputKey(vk, 0);
        }
        break;
    }
}

#else /* !_WIN32 */

void pc_input_host_key(int vk, int down)
{
    if (vk > 0 && vk < 256)
        key_event(vk, down);
}

void pc_input_host_wheel(int notches)
{
    if (notches)
        xbox_HostInputWheel(notches);
}

#endif /* _WIN32 */
