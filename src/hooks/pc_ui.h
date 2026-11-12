/* The launcher and the in-game overlay: one settings screen (pc_ui.cpp), two ways to reach it. */

#ifndef DEFJAM_PC_UI_H
#define DEFJAM_PC_UI_H

#include <stddef.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "recomp_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- settings and the decision to show the launcher (pc_ui_settings.c) ------ */

/* Fill `out` with the [launcher], [debug] and [ui] settings. Returns the count. */
size_t pc_ui_settings(RecompSetting *out, size_t max);

/* Whether to show the launcher before the game starts. It is shown on every
 * launch unless the player turned "skip" on, with these exceptions, in order:
 * a scripted or test run (RECOMP_SETTINGS=none, a pad script, RECOMP_NO_LAUNCHER)
 * never shows it; `--no-launcher` on the command line skips it; `--launcher`, or
 * Shift held at start, shows it even when it is skipped. */
int pc_launcher_wanted(int skip_setting, int force_flag, int no_flag, int shift_held,
                       int test_run);

#if defined(_WIN32)
/* ---- the in-game overlay (pc_ui.cpp) ------------------------------------------ */

/* The window the overlay draws into and takes input from. */
void pc_ui_set_window(HWND window);

/* The present step's overlay hook (toolkit d3d8_present_set_overlay). */
void pc_ui_overlay(void *device, void *context, void *window_rtv, unsigned width, unsigned height,
                   void *user);

/* Offer a window message to the overlay, on the window's thread. Returns non-zero
 * when the overlay used it (the overlay key, a key being captured for a binding)
 * and the message should go no further. */
int pc_ui_window_message(HWND window, UINT msg, WPARAM wp, LPARAM lp);

int pc_ui_overlay_open(void);

/* Read the overlay's hotkeys and size again after a setting changed. */
void pc_ui_apply_settings(void);

/* ---- the launcher (pc_launcher.cpp) ----------------------------------------------- */

/* Show the launcher and run it until the player plays or quits. Returns 1 to go
 * on and start the game, 0 to quit. */
int pc_launcher_run(void);

#else
/* ---- off Windows: SDL's window, the toolkit's Vulkan present (pc_ui.cpp, pc_launcher_sdl.cpp) ---- */

/* Offer an SDL event (an SDL_Event) to the overlay, on the thread that reads the window's events.
 * The window's size in points turns the pointer's position into the pixels the overlay is drawn
 * in. Returns non-zero when the overlay used it (its key, a key being captured for a binding) and
 * it should go no further. */
int pc_ui_sdl_event(const void *event, float window_width, float window_height);

int  pc_ui_overlay_open(void);
void pc_ui_apply_settings(void);

/* Hand the overlay to the toolkit's Vulkan present step (d3d8_vk_set_overlay). */
void pc_ui_register_overlay(void);

/* Show the launcher and run it until the player plays or quits. Returns 1 to go on and start the
 * game, 0 to quit. The window is SDL's, so this hands the work to the thread that owns the window
 * (host_posix_call_on_main) and waits. Returns 1 when there is no window to show it in. */
int pc_launcher_run(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
