#ifndef DEFJAM_SAVE_COMPAT_H
#define DEFJAM_SAVE_COMPAT_H

#include <stdint.h>

/* Read-only compatibility for folders created by the old SHRD count-zero bug. */
/* Pass the runtime-resolved UDATA host directory, encoded as UTF-8. */
void defjam_save_compat_init(const char *user_data_dir);
int defjam_save_directory_name(const uint16_t *name, char out[13]);

#endif
