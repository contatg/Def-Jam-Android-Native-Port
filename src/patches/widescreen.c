/*
 * Widescreen: settings, placement maths and the 3D view. See widescreen.h and
 * docs/widescreen/ (02-design.md is normative for the maths).
 */
#define _CRT_SECURE_NO_WARNINGS
#include "widescreen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "recomp/gen/recomp_types.h"
#include "recomp_settings.h"
#include "d3d8_present.h"
#include "hooks/recorder.h"

static float g_aspect;          /* 0 = off */
static float g_k = 1.0f;
static int g_hud = WS_HUD_EDGE;
static int g_cutscenes = WS_CUTSCENES_AUTHORED;
static int g_fill = WS_FILL_BLUR;
static float g_fov_cap = 150.0f;

float ws_aspect(void) { return g_aspect; }
float ws_k(void) { return g_k; }
int ws_hud_mode(void) { return g_hud; }
int ws_cutscene_mode(void) { return g_cutscenes; }
int ws_side_fill(void) { return g_fill; }
float ws_fov_cap(void) { return g_fov_cap; }

/* The choices of [display] widescreen, as aspects; "auto" is resolved from the
 * screen. The first release supports 16:9 and 16:10; wider ones are offered as
 * experimental (owner, 2026-10-11) and "auto" does not pick them. */
/* [display] aspect: 4:3, stretch, widescreen (auto), 16:9, 16:10, 21:9, 32:9 (src/hooks/pc_settings.c). */
static const float k_choice_aspect[] = { 0.0f, 0.0f, -1.0f, 16.0f / 9.0f, 16.0f / 10.0f, 21.0f / 9.0f, 32.0f / 9.0f };

static float snap_auto(float a)
{
    static const float known[] = { 4.0f / 3.0f, 16.0f / 10.0f, 16.0f / 9.0f };
    size_t i;
    if (a <= WS_DESIGN_ASPECT * 1.03f)
        return 0.0f;                                   /* 4:3 or narrower: nothing to widen */
    for (i = 0; i < sizeof known / sizeof known[0]; i++)
        if (fabsf(a - known[i]) <= known[i] * 0.03f)
            return known[i] <= WS_DESIGN_ASPECT ? 0.0f : known[i];
    return a > 16.0f / 9.0f ? 16.0f / 9.0f : a;        /* ultrawide only when chosen explicitly */
}

float ws_init(int window_w, int window_h, int fullscreen)
{
    const char *exp = getenv("RECOMP_WS_ASPECT");      /* experiments: any aspect, overrides the setting */
    int choice = recomp_settings_get("display", "aspect", 0);
    float a = 0.0f;

    if (exp && atof(exp) > 0.0) {
        a = (float)atof(exp);
    } else if (choice > 0 && choice < (int)(sizeof k_choice_aspect / sizeof k_choice_aspect[0])) {
        a = k_choice_aspect[choice];
        if (a < 0.0f) {                                /* auto */
            /* The screen's shape, windowed or not: a window's size is arbitrary,
             * and full screen is often entered after start-up (F11, Alt+Enter),
             * while the canvas is fixed for the run. Owner's play-test: a 1470x960
             * window gave a 1.53 canvas with bars on his 16:9 monitor. */
            int w = window_w, h = window_h;
            (void)fullscreen;
#if defined(_WIN32)
            {
                int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
                if (sw > 0 && sh > 0) {
                    w = sw;
                    h = sh;
                }
            }
#else
            {
                extern int host_posix_display_size(int *w, int *h);   /* src/host_posix.c, SDL */
                int sw, sh;
                if (host_posix_display_size(&sw, &sh)) {
                    w = sw;
                    h = sh;
                }
            }
#endif
            a = (w > 0 && h > 0) ? snap_auto((float)w / (float)h) : 0.0f;
        }
    }
    if (a <= WS_DESIGN_ASPECT)
        a = 0.0f;
    g_aspect = a;
    g_k = a > 0.0f ? WS_DESIGN_ASPECT / a : 1.0f;
    g_hud = recomp_settings_get("display", "widescreen_hud", 0);
    if (g_hud == WS_HUD_AUTO)
        g_hud = a > 16.0f / 9.0f * 1.01f ? WS_HUD_CENTERED : WS_HUD_EDGE;
    g_cutscenes = recomp_settings_get("display", "widescreen_cutscenes", 0);
    g_fill = recomp_settings_get("display", "widescreen_side_fill", 0);
    g_fov_cap = (float)recomp_settings_get("display", "widescreen_fov_cap", 150);

    if (a > 0.0f) {
        extern void nv2a_ws_configure(float k, float hud_reach);
        unsigned num = (unsigned)(a * 10000.0f + 0.5f);
        d3d8_present_set_canvas_aspect(num, 10000);
        /* HUD reach: the screen edge, or the edge of a centred 16:9 box. */
        nv2a_ws_configure(g_k, g_hud == WS_HUD_CENTERED && a > 16.0f / 9.0f ? (16.0f / 9.0f) / a : 1.0f);
        {
            extern void nv2a_ws_set_side_fill(int on);
            nv2a_ws_set_side_fill(g_fill == WS_FILL_BLUR);
        }
        fprintf(stderr, "[WS] widescreen %.4f (k %.4f), HUD %s, cutscenes %s, sides %s, FOV cap %.0f\n",
                a, g_k, g_hud == WS_HUD_EDGE ? "edge" : "centred",
                g_cutscenes == WS_CUTSCENES_FILL ? "fill" : "authored",
                g_fill == WS_FILL_BLACK ? "black" : "blur", g_fov_cap);
    } else {
        d3d8_present_set_canvas_aspect(0, 0);
    }
    return a;
}

float ws_widen_fov(float fov_deg, float from, float to, float cap)
{
    const double d2r = 3.14159265358979323846 / 180.0;
    double f;
    if (!(to > from))
        return fov_deg;
    f = 2.0 * atan(tan(fov_deg * d2r * 0.5) * (to / from)) / d2r;
    if (f > cap)
        f = cap;
    return (float)f;
}

void ws_view_params(uint32_t vp, uint32_t mode, uint32_t pfov, uint32_t pasp, uint32_t py, uint32_t ph)
{
    float fov = MEMF(vp + 0xC), x = MEMF(vp + 0x14), y = MEMF(vp + 0x18);
    float w = MEMF(vp + 0x1C), h = MEMF(vp + 0x20);
    float a = g_aspect;
    /* R2: only a view over the whole screen is widened. The title's device stays
     * 640 wide; a sub-rectangle (a fighter in a menu panel) keeps its 4:3 view
     * and is placed by the 2D layer instead. */
    int full = x <= 0.5f && w >= 639.5f;
    static unsigned logged[3];

    if (mode > 2)
        return;                                        /* the original writes nothing */
    /* Mode 1 is a Story cutscene's view: the recorder notes when one starts and ends. */
    if (full)
        recorder_set_view_mode((int)mode);
    if (mode == 2) {
        MEMF(pfov) = fov;
        if (a > 0.0f && !full)    /* a squeezed sub-view keeps the shape it was authored with */
            MEMF(pasp) = (float)((double)MEMF(vp + 0x1C) / (double)g_k / (double)h);
        else
            MEMF(pasp) = (float)((double)MEMF(vp + 0x1C) / (double)h);
        MEMF(py) = y;
        MEMF(ph) = h;
    } else if (mode == 1) {
        /* Cutscenes: the game's own 16:9 framing (a 640x360 letterbox at 4:3). */
        if (a <= 0.0f) {
            MEMF(pfov) = fov;
            MEM32(pasp) = 0x3FE38E39;                  /* 16/9 */
            MEMF(py) = (float)((double)h * 0.25 * 0.5 + (double)y);
            MEMF(ph) = (float)((double)h - (double)h * 0.25);
        } else if (g_cutscenes == WS_CUTSCENES_FILL || a >= WS_CUTSCENE_ASPECT) {
            /* Full height. At 16:9 this is the authored picture exactly; wider
             * (until the 16:9 box exists) and in "fill" it is Hor+ from 16:9,
             * and narrower in "fill" it shows a little more above and below. */
            MEMF(pfov) = ws_widen_fov(fov, WS_CUTSCENE_ASPECT, a, g_fov_cap);
            MEMF(pasp) = a;
            MEMF(py) = y;
            MEMF(ph) = h;
        } else {
            /* Narrower than 16:9 (16:10): the authored 16:9 picture with thin bars. */
            float hh = h * (a / WS_CUTSCENE_ASPECT);
            MEMF(pfov) = fov;
            MEM32(pasp) = 0x3FE38E39;
            MEMF(py) = y + (h - hh) * 0.5f;
            MEMF(ph) = hh;
        }
    } else {
        if (a > 0.0f && full) {
            MEMF(pfov) = ws_widen_fov(fov, WS_DESIGN_ASPECT, a, g_fov_cap);
            MEMF(pasp) = a;
        } else {
            MEMF(pfov) = fov;
            MEM32(pasp) = 0x3FAAAAAB;                  /* 4/3 */
        }
        MEMF(py) = y;
        MEMF(ph) = h;
    }
    if (a > 0.0f && logged[mode]++ < 4)
        fprintf(stderr, "[WS] view mode %u %s: fov %.2f -> %.2f, aspect %.4f, y %.1f h %.1f\n",
                mode, full ? "full" : "sub", fov, MEMF(pfov), MEMF(pasp), MEMF(py), MEMF(ph));
}

/* RECOMP_WS_TRACE2D=1 (investigation, docs/widescreen WS-4): who sets native 2D
 * quad positions and vertex offsets, with what values. Aggregated by call site
 * (the return address) and printed as a table every 20000 calls. Off: nothing. */
typedef struct { uint32_t site, fn, count, obj; float a, b, minx, maxx; } Ws2dSite;
static Ws2dSite s_sites[256];
static unsigned s_nsites, s_calls;

static void trace2d(uint32_t fn, uint32_t site, uint32_t obj, float a, float b)
{
    static int on = -1;
    unsigned i;
    if (on < 0) {
        const char *e = getenv("RECOMP_WS_TRACE2D");
        on = e && *e == '1';
    }
    if (!on)
        return;
    for (i = 0; i < s_nsites; i++)
        if (s_sites[i].site == site)
            break;
    if (i == s_nsites) {
        if (s_nsites == 256)
            return;
        s_nsites++;
        s_sites[i].site = site; s_sites[i].fn = fn;
        s_sites[i].minx = 1e9f; s_sites[i].maxx = -1e9f;
        fprintf(stderr, "[WS2D] new site fn %06X site %08X obj %08X (%.2f, %.2f) at call %u\n", fn, site, obj, a, b, s_calls);
    }
    s_sites[i].count++;
    s_sites[i].obj = obj; s_sites[i].a = a; s_sites[i].b = b;
    if (a < s_sites[i].minx) s_sites[i].minx = a;
    if (a > s_sites[i].maxx) s_sites[i].maxx = a;
    if (++s_calls % 1000 == 0) {
        fprintf(stderr, "[WS2D] %u calls, %u sites\n", s_calls, s_nsites);
        for (i = 0; i < s_nsites; i++)
            fprintf(stderr, "[WS2D]   fn %06X site %08X x%-7u obj %08X last (%.2f, %.2f) x range %.2f..%.2f\n",
                    s_sites[i].fn, s_sites[i].site, s_sites[i].count, s_sites[i].obj,
                    s_sites[i].a, s_sites[i].b, s_sites[i].minx, s_sites[i].maxx);
        fflush(stderr);
    }
}

void ws_trace_pos(void)    { trace2d(0x28F90, MEM32(g_esp), g_ecx, MEMF(g_esp + 4), MEMF(g_esp + 8)); }
void ws_trace_vtx_arr(void)
{
    uint32_t i = MEM32(g_esp + 4), arr = MEM32(g_esp + 8);
    trace2d(0x2E390, MEM32(g_esp), g_ecx, MEMF(arr + i * 12), MEMF(arr + i * 12 + 4));
}
void ws_trace_vtx(void)    { trace2d(0x2E470, MEM32(g_esp), g_ecx, MEMF(g_esp + 8), MEMF(g_esp + 12)); }

/* Sub-views (3D inside a menu panel: the sketch artist's head, a created
 * fighter's portrait on the versus poster, menu previews). sub_0001A7A0 is the
 * viewport's SetRect(this, x, y, w, h, apply), thiscall with five stack words.
 * The panels around them are centred and squeezed by k in the renderer, so a
 * rectangle that is not the whole screen is squeezed the same way here, before
 * the game stores it; the 3D then renders exactly where its panel is. Its
 * projection keeps the authored shape (ws_view_params). Full-screen rectangles
 * are left alone. Off: nothing. */
void ws_view_rect_enter(void)
{
    float x = MEMF(g_esp + 4), w = MEMF(g_esp + 12);
    static unsigned logged;
    if (g_aspect <= 0.0f || (x <= 0.5f && w >= 639.5f))
        return;
    MEMF(g_esp + 4) = 320.0f + (x - 320.0f) * g_k;
    MEMF(g_esp + 12) = w * g_k;
    if (logged++ < 8)
        fprintf(stderr, "[WS] sub-view rect x %.1f w %.1f -> x %.1f w %.1f\n", x, w, MEMF(g_esp + 4), MEMF(g_esp + 12));
}
