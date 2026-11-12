/* Where the picture goes in the window: d3d8_present_fit. */

#include "d3d8_present.h"

#include <stdio.h>

static int g_failed;

static void expect(const char *name, D3D8PresentRect r,
                   unsigned x, unsigned y, unsigned w, unsigned h)
{
    if (r.x != x || r.y != y || r.width != w || r.height != h) {
        printf("FAIL %s: got %u,%u %ux%u, want %u,%u %ux%u\n", name,
               r.x, r.y, r.width, r.height, x, y, w, h);
        g_failed++;
    }
}

int main(void)
{
    /* The same shape fills the window. */
    expect("4:3 in 640x480", d3d8_present_fit(4, 3, 640, 480, 1), 0, 0, 640, 480);
    expect("4:3 in 1280x960", d3d8_present_fit(4, 3, 1280, 960, 1), 0, 0, 1280, 960);
    /* Wider windows get bars at the sides. */
    expect("4:3 in 1920x1080", d3d8_present_fit(4, 3, 1920, 1080, 1), 240, 0, 1440, 1080);
    expect("4:3 in 2560x1440", d3d8_present_fit(4, 3, 2560, 1440, 1), 320, 0, 1920, 1440);
    expect("4:3 in 3840x2160", d3d8_present_fit(4, 3, 3840, 2160, 1), 480, 0, 2880, 2160);
    expect("4:3 in 1280x800", d3d8_present_fit(4, 3, 1280, 800, 1), 106, 0, 1067, 800);
    /* Taller windows get bars above and below. */
    expect("4:3 in 600x800", d3d8_present_fit(4, 3, 600, 800, 1), 0, 175, 600, 450);
    expect("16:9 in 1280x960", d3d8_present_fit(16, 9, 1280, 960, 1), 0, 120, 1280, 720);
    /* Stretch, and degenerate input, use the whole window. */
    expect("stretch", d3d8_present_fit(4, 3, 1920, 1080, 0), 0, 0, 1920, 1080);
    expect("zero ratio", d3d8_present_fit(0, 3, 1920, 1080, 1), 0, 0, 1920, 1080);
    expect("empty window", d3d8_present_fit(4, 3, 0, 0, 1), 0, 0, 0, 0);
    /* Never empty, never outside the window. */
    expect("sliver", d3d8_present_fit(4, 3, 1000, 1, 1), 499, 0, 1, 1);
    expect("column", d3d8_present_fit(4, 3, 1, 1000, 1), 0, 499, 1, 1);

    /* The widened render target: height times the canvas aspect, even, never narrower than the title. */
    {
        static const unsigned cases[][6] = {
            /* lw, lh, scale, num, den, want */
            { 640, 480, 2, 0, 0, 1280 },        /* no canvas */
            { 640, 480, 2, 4, 3, 1280 },        /* the title's own shape */
            { 640, 480, 1, 16, 9, 854 },        /* 853.3 -> 854 */
            { 640, 480, 2, 16, 9, 1708 },       /* 1706.7 -> 1708 (rounded to even) */
            { 640, 480, 2, 16, 10, 1536 },
            { 640, 480, 4, 21, 9, 4480 },
            { 640, 480, 4, 32, 9, 6828 },
            { 640, 480, 2, 5, 4, 1280 },        /* narrower than 4:3: unchanged */
            { 640, 480, 2, 1, 0, 1280 },        /* degenerate */
            { 640, 480, 4, 1000, 1, 2560 },     /* beyond the texture limit: unchanged */
        };
        unsigned i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            unsigned got = d3d8_canvas_width(cases[i][0], cases[i][1], cases[i][2], cases[i][3], cases[i][4]);
            if (got != cases[i][5]) {
                printf("FAIL canvas %u:%u at scale %u: got %u, want %u\n", cases[i][3], cases[i][4],
                       cases[i][2], got, cases[i][5]);
                g_failed++;
            }
        }
    }

    /* Which displays can show 60 frames a second evenly. */
    {
        static const unsigned cases[][2] = {
            { 60, 1 }, { 59, 1 }, { 61, 1 }, { 120, 2 }, { 119, 2 }, { 180, 3 }, { 240, 4 }, { 239, 4 },
            { 90, 0 }, { 144, 0 }, { 165, 0 }, { 75, 0 }, { 50, 0 }, { 30, 0 }, { 300, 0 }, { 360, 0 }, { 0, 0 },
        };
        unsigned i;
        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            if (d3d8_present_interval_for(cases[i][0]) != cases[i][1]) {
                printf("FAIL interval for %u Hz: got %u, want %u\n", cases[i][0],
                       d3d8_present_interval_for(cases[i][0]), cases[i][1]);
                g_failed++;
            }
    }

    if (g_failed) { printf("%d case(s) failed\n", g_failed); return 1; }
    printf("d3d8_present_fit: all cases passed\n");
    return 0;
}
