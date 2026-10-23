/* The port's player-facing settings and start-up paths. See pc_settings.c. */

#ifndef DEFJAM_PC_SETTINGS_H
#define DEFJAM_PC_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Call first thing in WinMain: finds the game folder, makes it the working
 * directory's `game`, opens a log when nothing captures the output, supplies
 * the runtime's required switches, then loads settings.ini. Returns 0 when no
 * game folder could be found (a message has been shown). */
int pc_settings_init(void);

/* The command line, for the options that are not settings (--launcher, --no-launcher). Off
 * Windows only: there the program is started through main, which has it. */
void pc_settings_set_args(int argc, char **argv);

/* Effective value of a [display] setting. */
int pc_display(const char *key, int fallback);

/* Store a [display] value and write the file. Safe from any thread. */
void pc_display_set(const char *key, int value);

/* Push the display settings that the renderer owns to it. Call before the
 * Direct3D device is created, and it is called again on every change. */
void pc_settings_apply_display(void);

/* The window asks to be told when display.fullscreen changes. */
void pc_settings_on_fullscreen(void (*notify)(int fullscreen));

/* The window asks to be told when [display] window_width or window_height changed. */
void pc_settings_on_window_size(void (*notify)(int width, int height));

/* The settings file's path ("" when settings are not kept: RECOMP_SETTINGS=none), and writing
 * the current values to it. The launcher and overlay save as the player changes things. */
const char *pc_settings_path(void);
void pc_settings_save(void);

#ifdef __cplusplus
}
#endif

#endif
