/* Picture options: brightness and the venue vignette ([display] brightness, vignette,
 * vignette_strength). Both apply as they change, in the launcher or the overlay.
 *
 * Brightness is a power curve on top of the title's own gamma ramp (src/recomp_manual.c,
 * sub_002203C6_enter): out = in^(100/brightness), so black and white stay where they are and
 * the mid-tones move. 100 leaves the title's ramp exactly as the console has it. The result is
 * handed to the presentation pass (d3d8_gamma_set) and kept in g_recomp_gamma for captures.
 *
 * The vignette is part of the venue post effects (CPostEffectMan, assets/postfx/<venue>.pfx;
 * docs/research/picture-options.md). Each frame sub_000D8C40 calls the platform tint's draw,
 * sub_000D9AF0(this, modR, modG, modB, fillR, fillG, fillB, vignette), thiscall, `ret 0x1C`;
 * the last argument is the variance's ScreenTintVignetteEnable. With it set the draw puts the
 * "Vignette Quad" (this+4) over the whole screen with blend preset 4, a multiply
 * (src DESTCOLOR, dst ZERO), and colour 0xFF in all four channels through sub_00028460.
 *
 *   off            the argument is cleared, so the quad is not drawn at all;
 *   strength s<1   the quad's destination factor becomes INVSRCALPHA and its colour s in all
 *                  four channels: dst * (s * vignette + 1 - s), the game's picture blended
 *                  towards the undarkened one. This assumes an opaque vignette texture; the
 *                  measured captures agree with it to a few levels.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "recomp/gen/recomp_types.h"
#include "recomp_settings.h"
#include "picture.h"

extern void d3d8_gamma_set(const void *ramp);

uint8_t g_recomp_gamma[3][256];
int g_recomp_gamma_on;

static uint8_t s_title[3][256];
static int s_have_title;        /* the title has set its ramp (and [display] gamma is on) */
static int s_applied;           /* a ramp has been handed to the presentation pass */
static int s_brightness = 100;

static volatile int s_vignette_alpha = 255;     /* 0: not drawn */
static uint32_t s_fade_quad;                    /* the vignette quad of the draw in progress */

static void compose(void)
{
    struct { uint16_t c[3][256]; } ramp;
    double e = 100.0 / (double)s_brightness;
    int c, i, on = 0;

    if (!s_have_title && s_brightness == 100 && !s_applied)
        return;     /* nothing set yet: leave the picture exactly as drawn */
    for (c = 0; c < 3; c++)
        for (i = 0; i < 256; i++) {
            unsigned v = s_have_title ? s_title[c][i] : (unsigned)i;
            unsigned w = v * 257u;
            if (s_brightness != 100)
                w = (unsigned)(65535.0 * pow((double)v / 255.0, e) + 0.5);
            ramp.c[c][i] = (uint16_t)w;
            g_recomp_gamma[c][i] = (uint8_t)((w + 128u) / 257u);
            if (w != (unsigned)i * 257u)
                on = 1;
        }
    g_recomp_gamma_on = on;
    s_applied = 1;
    d3d8_gamma_set(&ramp);
}

void picture_title_ramp(const uint8_t *ramp)
{
    int c, i;
    if (ramp) {
        for (c = 0; c < 3; c++)
            for (i = 0; i < 256; i++)
                s_title[c][i] = ramp[c * 256 + i];
        s_have_title = 1;
    }
    compose();
}

void picture_apply_settings(void)
{
    int b = recomp_settings_get("display", "brightness", 100);
    int strength = recomp_settings_get("display", "vignette_strength", 100);
    static int logged = -1;

    if (b < 50) b = 50;
    if (b > 200) b = 200;
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;
    s_vignette_alpha = recomp_settings_get("display", "vignette", 1) ? (strength * 255 + 50) / 100 : 0;
    if (b != s_brightness) {
        s_brightness = b;
        compose();
    }
    if (logged != s_brightness * 1000 + s_vignette_alpha) {
        logged = s_brightness * 1000 + s_vignette_alpha;
        fprintf(stderr, "[PICTURE] brightness %d, vignette %s (alpha %d)\n", s_brightness,
                s_vignette_alpha ? "on" : "off", s_vignette_alpha);
    }
}

/* sub_000D9AF0_enter: the arguments are above the return address at g_esp. */
void picture_tint_draw_enter(void)
{
    static int reported = -1;
    int alpha = s_vignette_alpha;
    s_fade_quad = 0;
    if (!MEM8(g_esp + 0x1C))
        return;
    if (reported != alpha) {        /* once a setting: whether a scene draws one at all */
        reported = alpha;
        fprintf(stderr, "[PICTURE] venue vignette %s\n",
                alpha == 0 ? "removed" : alpha < 255 ? "faded" : "drawn as authored");
    }
    if (alpha == 0)
        MEM8(g_esp + 0x1C) = 0;
    else if (alpha < 255)
        s_fade_quad = MEM32(g_ecx + 4);
}

/* sub_00028460_enter: a quad's colour, (this, four channel bytes as dwords), `ret 0x10`. The
 * draw above has just set the vignette quad's blend preset; its state block (quad+0x14) keeps
 * the factors at +0x34 (source) and +0x38 (destination). */
void picture_quad_color_enter(void)
{
    uint32_t state, alpha;
    if (!s_fade_quad || g_ecx != s_fade_quad)
        return;
    s_fade_quad = 0;
    state = MEM32(g_ecx + 0x14);
    if (state < 0x10000u || MEM32(state + 0x34) != 0x306u || MEM32(state + 0x38) != 0)
        return;     /* not the multiply this was written for: leave it */
    alpha = (uint32_t)s_vignette_alpha;
    MEM32(state + 0x38) = 0x303u;   /* D3DBLEND_INVSRCALPHA */
    MEM32(g_esp + 4) = alpha;
    MEM32(g_esp + 8) = alpha;
    MEM32(g_esp + 12) = alpha;
    MEM32(g_esp + 16) = alpha;
}
