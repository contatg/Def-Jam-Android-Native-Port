/* The [debug] settings and the play-test recorder's start-up. See pc_debug.c. */

#ifndef DEFJAM_PC_DEBUG_H
#define DEFJAM_PC_DEBUG_H

#include <stddef.h>
#include "recomp_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The [debug] section's table, appended to the settings like the input tables. */
size_t pc_debug_settings(RecompSetting *out, size_t max);

/* Start recording when [debug] record is on, RECOMP_RECORD=1 or `force` (the
 * --record command-line flag). `log_path` is
 * the log this program opened for itself, or NULL when the caller redirected
 * stderr (the file is then looked up). Call after the settings and the
 * launcher, before the audio starts. */
void pc_debug_start(const char *log_path, int force);

#ifdef __cplusplus
}
#endif

#endif
