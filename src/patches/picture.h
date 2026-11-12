/* Picture options: brightness and the venue vignette. See picture.c. */
#ifndef DEFJAM_PICTURE_H
#define DEFJAM_PICTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The title's gamma ramp (256 bytes each of red, green, blue), or NULL to apply only the
 * brightness (the title's ramp is off). Hands the composed ramp to the presentation pass. */
void picture_title_ramp(const uint8_t *ramp);

/* Read [display] brightness, vignette and vignette_strength; call at start-up and on a change. */
void picture_apply_settings(void);

/* Guest hooks (src/recomp_manual.c). */
void picture_tint_draw_enter(void);
void picture_quad_color_enter(void);

#ifdef __cplusplus
}
#endif

#endif
