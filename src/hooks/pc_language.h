/* Language packs: translated text, subtitles, fonts and menu art (docs/language-packs.md). */

#ifndef DEFJAM_PC_LANGUAGE_H
#define DEFJAM_PC_LANGUAGE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Register [language] pack, story_subtitles and game. Returns how many were added. */
struct RecompSetting;
size_t pc_language_settings(struct RecompSetting *out, size_t max);

/* After the launcher: load the chosen language mod from the mods folder
 * (RECOMP_TEXTURE_ROOT, else <data_root>/mods), rebuild its
 * files from the dump in game_dir (once; kept in <data_root>/cache/language) and
 * serve them in place of the originals. A pack that cannot be used is reported
 * and the game runs in its own language. data_root may be NULL. */
void pc_language_start(const char *data_root, const char *game_dir);

/* Apply the pack's change to default.xbe's data sections to the loaded image,
 * before the memory layout maps it. Nothing when no pack changes the XBE. */
void pc_language_patch_xbe(void *xbe, size_t size);

/* Whether every Story cutscene should show subtitles: [language] story_subtitles,
 * or with "auto" what the active pack asks for (src/patches/subtitles.c). */
int pc_language_all_subtitles(void);

/* Whether the game should start in its own French ([language] game = french): never while a
 * language mod is on, which keeps the English slot it translates (src/patches/game_language.c). */
int pc_language_french(void);

#ifdef __cplusplus
}
#endif

#endif
