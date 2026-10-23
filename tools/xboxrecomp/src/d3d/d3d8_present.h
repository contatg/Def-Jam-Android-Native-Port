/*
 * Scaled presentation: the title renders into a target of its own size and
 * that picture is drawn into a window of any size.
 *
 * Without this the swap chain is the title's size times the render scale and
 * DXGI stretches it over whatever the window happens to be, so a resized or
 * full-screen window distorts the picture. With it the swap chain follows the
 * window, the title's target stays what it was, and each present draws one
 * into the other: filling the window, or at a fixed aspect ratio with black
 * bars.
 *
 * Opt-in. A host that does not call d3d8_present_enable_scaling gets the
 * swap chain and the behaviour it always had.
 *
 * Plain C types only, so the fitting arithmetic can be tested without a
 * Direct3D device.
 */

#ifndef D3D8_PRESENT_H
#define D3D8_PRESENT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D8PresentRect {
    unsigned x, y, width, height;
} D3D8PresentRect;

/* Where a picture of aspect num:den goes in a dst_w x dst_h window: centred
 * and as large as fits when `keep` is set, the whole window otherwise. A
 * degenerate ratio or window yields the whole window. */
static inline D3D8PresentRect d3d8_present_fit(unsigned num, unsigned den,
                                               unsigned dst_w, unsigned dst_h, int keep)
{
    D3D8PresentRect r;
    r.x = 0; r.y = 0; r.width = dst_w; r.height = dst_h;
    if (!keep || !num || !den || !dst_w || !dst_h)
        return r;
    if ((unsigned long long)dst_w * den > (unsigned long long)dst_h * num) {
        /* Window wider than the picture: bars left and right. */
        r.width = (unsigned)(((unsigned long long)dst_h * num + den / 2) / den);
        if (r.width > dst_w) r.width = dst_w;
        if (!r.width) r.width = 1;
        r.x = (dst_w - r.width) / 2;
    } else {
        /* Window taller: bars above and below. */
        r.height = (unsigned)(((unsigned long long)dst_w * den + num / 2) / num);
        if (r.height > dst_h) r.height = dst_h;
        if (!r.height) r.height = 1;
        r.y = (dst_h - r.height) / 2;
    }
    return r;
}

/* Width in pixels of the title's render target when it is widened to a canvas
 * of aspect num:den. The title keeps its own logical size lw x lh; the target
 * is lh * scale high and as wide as the canvas aspect asks, rounded to an even
 * number, so the title's x coordinates are spread over a wider picture (the
 * title widens its own projection to match). A canvas no wider than the
 * title's own aspect, or a degenerate ratio, gives the usual lw * scale. */
static inline unsigned d3d8_canvas_width(unsigned lw, unsigned lh, unsigned scale,
                                         unsigned num, unsigned den)
{
    unsigned long long w;
    unsigned base = lw * scale;
    if (!num || !den || !lw || !lh || (unsigned long long)num * lh <= (unsigned long long)den * lw)
        return base;
    w = ((unsigned long long)lh * scale * num + den / 2) / den;
    w = (w + 1) & ~1ull;
    return w > base && w <= 16384 ? (unsigned)w : base;
}

/* Call before the device is created. */
void d3d8_present_enable_scaling(int enable);

/* Widen the title's render target to a canvas of aspect num:den (see
 * d3d8_canvas_width); 0, 0 keeps the title's own shape. Only with scaling
 * enabled. Call before the device is created. */
void d3d8_present_set_canvas_aspect(unsigned num, unsigned den);

/* The render scale to create the device with (1-4). 0 leaves the choice to
 * RECOMP_RENDER_SCALE and its default. Call before the device is created. */
void d3d8_present_set_render_scale(unsigned scale);

/* The following take effect at the next present, from any thread. */

/* keep = 1: show the picture at num:den with bars. keep = 0: fill the window. */
void d3d8_present_set_aspect(int keep, unsigned num, unsigned den);

/* 1 paces presentation on the display when it can show 60 frames a second
 * evenly -- a refresh rate that is a multiple of 60, presenting every 1 to 4
 * refreshes -- and does not wait otherwise (90, 144, 165 Hz: a title frame
 * would land on an uneven number of refreshes). 0 never waits. -1 leaves the
 * choice to RECOMP_PRESENT_VSYNC, which waits one refresh. */
void d3d8_present_set_vsync(int vsync);

/* 1 while presents are being paced on the display, which makes the display
 * the title's frame clock. A host whose title is otherwise paced by a timer
 * should relax that timer while this holds (xbox_Nv2aSetFlipHz), or the two
 * clocks drift against each other and frames are repeated or dropped. */
int d3d8_present_display_paced(void);

/* The sync interval for a display refreshing at `hz` (as Windows reports it,
 * so 59 for 59.94): hz / 60 when that is 1 to 4 and hz is within 1 of a
 * multiple of 60, else 0. */
static inline unsigned d3d8_present_interval_for(unsigned hz)
{
    unsigned k = (hz + 30) / 60;
    unsigned nearest = k * 60;
    unsigned off = hz > nearest ? hz - nearest : nearest - hz;
    return (k >= 1 && k <= 4 && off <= 1) ? k : 0;
}

/* 1 = smooth (bilinear), 0 = sharp (nearest). */
void d3d8_present_set_linear_filter(int linear);

/* Recent Vulkan presentation rate, sampled over roughly two seconds. */
float d3d8_present_fps(void);

/* A host program's drawing on top of the picture, in window pixels, after the
 * title's target has been scaled into the window: a menu, an on-screen display.
 * It runs on the thread that presents, on the immediate context (the pointers are Direct3D 11 interfaces; plain
 * pointers here keep this header free of Direct3D types), with the
 * window's back buffer as the render target; it must leave nothing bound that
 * the title's pipeline depends on (the title's own state is restored after the
 * blit, not after this). The picture a harness captures is the title's target,
 * so nothing drawn here is ever in a capture. */
typedef void (*D3D8PresentOverlay)(void *device /* ID3D11Device */, void *context /* ID3D11DeviceContext */,
                                   void *window /* ID3D11RenderTargetView */, unsigned width, unsigned height,
                                   void *user);
void d3d8_present_set_overlay(D3D8PresentOverlay overlay, void *user);

#ifdef __cplusplus
}
#endif

#endif /* D3D8_PRESENT_H */
