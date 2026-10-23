/* The port's input settings and window plumbing. See pc_input.c. */

#ifndef DEFJAM_PC_INPUT_H
#define DEFJAM_PC_INPUT_H

#include <stddef.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include "recomp_settings.h"
#include "input_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fill `out` with the [input], [gamepad] and [keyboard] settings (the caller
 * registers them with the display ones, in one table). Returns the count. */
size_t pc_input_settings(RecompSetting *out, size_t max);

/* Start the host input layer from the loaded settings: opens the pads, decides
 * how many controllers the title sees and sets RECOMP_USB_PADS to match unless
 * it is already set. Does nothing when the run is scripted or uses
 * RECOMP_SETTINGS=none, where the toolkit's own XInput path stays in charge.
 * Call once after the settings are loaded and before the USB model starts. */
void pc_input_start(void);

/* Stop the host input layer and any rumble. Called at exit, and by the window
 * before it ends the process, so a pad is not left vibrating. Safe to call
 * when the layer never started. */
void pc_input_stop(void);

/* An input setting changed: push the pad map, bindings and rumble strength to
 * the running layer. Safe from any thread. */
void pc_input_apply(void);

/* Offer a window message to the input layer: keys, mouse buttons, the wheel
 * and focus changes. Never consumes anything; the window procedure goes on to
 * handle the message as it did. Call from the window's own thread. */
#if defined(_WIN32)
void pc_input_window_message(HWND window, UINT msg, WPARAM wp, LPARAM lp);
#else
/* The same for a window that is not Win32's (src/host_posix.c): a key or
 * mouse button by its Windows virtual-key code, and wheel notches (positive
 * is away from the user). */
void pc_input_host_key(int vk, int down);
void pc_input_host_wheel(int notches);
#endif

/* Tell the layer whether the window has the focus right now (at creation). */
void pc_input_set_focus(int focused);

/* The virtual-key code a key message is for, with Shift, Ctrl and Alt resolved to the
 * side that was pressed (VK_LSHIFT and so on). Used to learn a key for a binding. */
#if defined(_WIN32)
int pc_input_vk_from_message(WPARAM wp, LPARAM lp);
#endif

/* The pad map the settings describe, for a screen that shows what the game sees. */
void pc_input_current_padmap(InputPadMap *map);

/* Stop and start the host input layer again from the settings, so changes that are read
 * only at start (who plays, how many controllers, the backend) take effect. Used after
 * the launcher. */
void pc_input_restart(void);

#ifdef __cplusplus
}
#endif

#endif
