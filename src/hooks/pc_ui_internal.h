// What the launcher and the overlay share: the settings screen and the way it takes input.
#pragma once

#if defined(_WIN32)
#include <windows.h>
#endif
#include "imgui.h"

#if defined(_WIN32)
// The Win32 backend's message handler (its header only declares it in a comment).
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

typedef HWND UiWindow;
#else
// Off Windows the window is SDL's and its events are fed in by hand (pc_ui.cpp): ui_sdl_event.
union SDL_Event;
typedef void *UiWindow;     // an SDL_Window *, or null

// The few virtual-key codes the screen names (the host input layer's bindings are written in them).
// (The host's win32_compat.h defines some of them; each is guarded on its own.)
#ifndef VK_LBUTTON
#define VK_LBUTTON  0x01
#endif
#ifndef VK_RBUTTON
#define VK_RBUTTON  0x02
#endif
#ifndef VK_MBUTTON
#define VK_MBUTTON  0x04
#endif
#ifndef VK_XBUTTON1
#define VK_XBUTTON1 0x05
#endif
#ifndef VK_XBUTTON2
#define VK_XBUTTON2 0x06
#endif
#ifndef VK_MENU
#define VK_MENU     0x12
#endif
#ifndef VK_ESCAPE
#define VK_ESCAPE   0x1B
#endif
#ifndef VK_F1
#define VK_F1       0x70
#endif
#ifndef VK_F11
#define VK_F11      0x7A
#endif

// A key by where it is on the keyboard, as the virtual-key code the bindings use (src/host_posix.c).
extern "C" int host_posix_vk_from_scancode(int sdl_scancode);
#endif

enum UiMode { UI_OVERLAY, UI_LAUNCHER };

struct UiResult {
    bool play = false;      // launcher: the Play button
    bool setup = false;     // open setup after launcher teardown
    bool leave = false;     // launcher: Quit; overlay: Close
};

// A fresh ImGui context with the port's fonts and style, scaled for `window`'s display.
void ui_context_begin(UiWindow window);
void ui_context_end();

// Make the text and controls the right size for `window` (display scaling times [ui] scale).
void ui_apply_scale(UiWindow window);

// Once a frame, before ImGui::NewFrame: every pad's buttons become the menu's gamepad navigation.
void ui_feed_gamepad();

#if defined(_WIN32)
// A key, mouse button or wheel notch being learned for a binding takes the message. Returns true
// when it did, and the message should go no further. Window thread; call with the UI lock held.
bool ui_capture_message(HWND window, UINT msg, WPARAM wp, LPARAM lp);
#else
// The same for an SDL event. `ui_sdl_event` also gives the event to ImGui (the keyboard, the mouse
// and typed text), with the pointer's position times the scales (window points to the pixels the
// screen is drawn in). Call with the UI lock held. Returns true when a capture took the event.
bool ui_capture_sdl_event(const union SDL_Event *event);
bool ui_sdl_event(const union SDL_Event *event, float scale_x, float scale_y);

// Once a frame, before ImGui::NewFrame: the size of the screen in ImGui's units, the pixel density
// behind them and the time since the last frame.
void ui_sdl_frame(float width, float height, float framebuffer_scale);

// The text size scale for a screen of this many pixels high (the overlay), or 1.
void ui_set_extra_scale(float scale);
#endif

// The settings screen, drawn into the whole of the current frame (launcher) or a panel (overlay).
UiResult ui_draw(UiMode mode, float x, float y, float width, float height);

// The current frame's pad-press detection runs here, so it is only done while a screen is showing.
bool ui_capturing();

// The lock that keeps the window thread's input events and the drawing thread out of ImGui at once.
void ui_lock();
void ui_unlock();

void ui_target(const char *name);
bool ui_test_target(const char *name, float *x, float *y);
