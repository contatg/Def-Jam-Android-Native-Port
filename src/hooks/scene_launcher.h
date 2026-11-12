/* Story scenes on demand through the game's developer cinema mode. See scene_launcher.c. */

#ifndef DEFJAM_SCENE_LAUNCHER_H
#define DEFJAM_SCENE_LAUNCHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's scenes, by number (0 sc00 ... 37 gf3c), read from its own table. */
int  scene_launcher_count(void);
int  scene_launcher_name(int index, char *out, size_t size);
int  scene_launcher_find(const char *name_or_number);          /* -1 when the game has no such scene */

/* Play scene `index` at the next frame of the 3D module (any thread may ask). */
void scene_launcher_request(int index);
int  scene_launcher_playing(void);                              /* the scene playing, -1 none */
unsigned scene_launcher_updates(void);                          /* moves while the 3D module runs */

/* _enter hooks (src/recomp_manual.c): the scene loader and the mode update. */
void scene_launcher_loader_enter(void);
void scene_launcher_mode_enter(void);

#ifdef __cplusplus
}
#endif

#endif
