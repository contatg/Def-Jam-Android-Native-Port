/*
 * d3d8_readback fixture: the asynchronous read-back (d3d8_readback.h) against
 * IDirect3DSurface8::LockRect on the back buffer, on the device this platform
 * builds (Direct3D 11 with a hidden window on Windows, Vulkan off screen
 * elsewhere; the OpenGL device must answer "unsupported").
 *
 * Each frame clears to its own colour and draws a triangle, takes the LockRect
 * picture as the reference, starts an asynchronous copy tagged with the frame
 * number and presents. Polled afterwards, every tag must come back once, in
 * order, with the reference's bytes and size. Run with "scaled", the device
 * presents through a scaled target at render scale 2 (the Direct3D 11 game texture
 * instead of the swap chain; a 1280x960 target everywhere). Also: a busy ring
 * answers 0, and Vulkan,
 * which completes a copy only with a submission, has nothing before Present.
 * Last, the time a frame spends capturing each way is printed (not judged:
 * a software device on a shared runner is no measurement).
 */
#include "d3d8_xbox.h"
#include "d3d8_readback.h"
#include "d3d8_present.h"

#ifndef _WIN32
#include "win32_compat.h"   /* QueryPerformanceCounter */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 640
#define H 480
#define TIMED 40
#define FRAMES 8

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

typedef struct { float x, y, z, rhw; DWORD color; float u, v; } Vertex;   /* 28 bytes */

static uint8_t *g_ref[FRAMES];
static uint32_t g_ref_w, g_ref_h;   /* the back buffer's size: the title's times the render scale */

static void draw_frame(IDirect3DDevice8 *dev, int k)
{
    Vertex tri[3] = {
        { 40.0f + 30.0f * (float)k, 40.0f, 0.5f, 1.0f, 0xFFFF0000u, 0.0f, 0.0f },
        { (float)W - 40.0f, (float)H / 2.0f, 0.5f, 1.0f, 0xFF00FF00u, 0.0f, 0.0f },
        { 80.0f, (float)H - 40.0f - 10.0f * (float)k, 0.5f, 1.0f, 0xFF0000FFu, 0.0f, 0.0f },
    };
    D3DCOLOR clear = 0xFF000000u | ((DWORD)(k * 29 + 10) << 16) | ((DWORD)(250 - k * 23) << 8) | (DWORD)(k * 13);
    dev->lpVtbl->Clear(dev, 0, NULL, D3DCLEAR_TARGET, clear, 1.0f, 0);
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    dev->lpVtbl->SetTexture(dev, 0, NULL);
    dev->lpVtbl->DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]);
}

/* The back buffer as LockRect gives it, copied out. */
static int lock_reference(IDirect3DDevice8 *dev, int k)
{
    IDirect3DSurface8 *bb = NULL;
    D3DLOCKED_RECT lr;
    int y;

    D3DSURFACE_DESC desc;

    if (dev->lpVtbl->GetBackBuffer(dev, 0, 0, &bb) != 0 || !bb)
        return 0;
    if (bb->lpVtbl->GetDesc(bb, &desc) != 0 || !desc.Width || !desc.Height
            || bb->lpVtbl->LockRect(bb, &lr, NULL, D3DLOCK_READONLY) != 0) {
        bb->lpVtbl->Release(bb);
        return 0;
    }
    g_ref_w = desc.Width;
    g_ref_h = desc.Height;
    g_ref[k] = malloc((size_t)g_ref_w * g_ref_h * 4);
    for (y = 0; y < (int)g_ref_h; y++)
        memcpy(g_ref[k] + (size_t)y * g_ref_w * 4, (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch, g_ref_w * 4);
    bb->lpVtbl->UnlockRect(bb);
    bb->lpVtbl->Release(bb);
    return 1;
}

static int compare(const D3D8Readback *rb)
{
    int k = (int)rb->tag, y;
    if (k < 0 || k >= FRAMES || !g_ref[k])
        return 0;
    if (rb->width != g_ref_w || rb->height != g_ref_h) {
        printf("frame %d: asynchronous %ux%u, LockRect %ux%u\n", k, rb->width, rb->height, g_ref_w, g_ref_h);
        return 0;
    }
    for (y = 0; y < (int)g_ref_h; y++)
        if (memcmp(rb->pixels + (size_t)y * rb->pitch, g_ref[k] + (size_t)y * g_ref_w * 4, g_ref_w * 4) != 0)
            return 0;
    return 1;
}

static double now_ms(void)
{
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}
static double median(double *v, int n) { qsort(v, (size_t)n, sizeof *v, cmp_double); return v[n / 2]; }

/* Median milliseconds the capture itself takes in a frame: LockRect of the back
 * buffer, or begin plus the poll after Present. */
static void timing(IDirect3DDevice8 *dev)
{
    double sync_ms[TIMED], async_ms[TIMED], sync_frame[TIMED], async_frame[TIMED], none_frame[TIMED], t, f;
    D3D8Readback rb;
    int k;

    for (k = 0; k < TIMED; k++) {
        f = now_ms();
        draw_frame(dev, k % FRAMES);
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        none_frame[k] = now_ms() - f;
    }
    for (k = 0; k < TIMED; k++) {
        IDirect3DSurface8 *bb = NULL;
        D3DLOCKED_RECT lr;
        f = now_ms();
        draw_frame(dev, k % FRAMES);
        t = now_ms();
        if (dev->lpVtbl->GetBackBuffer(dev, 0, 0, &bb) == 0 && bb) {
            if (bb->lpVtbl->LockRect(bb, &lr, NULL, D3DLOCK_READONLY) == 0)
                bb->lpVtbl->UnlockRect(bb);
            bb->lpVtbl->Release(bb);
        }
        sync_ms[k] = now_ms() - t;
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        sync_frame[k] = now_ms() - f;
    }
    for (k = 0; k < TIMED; k++) {
        double a, b;
        f = now_ms();
        draw_frame(dev, k % FRAMES);
        t = now_ms();
        d3d8_readback_begin((uint64_t)k);
        a = now_ms() - t;
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        t = now_ms();
        while (d3d8_readback_poll(&rb)) { }
        b = now_ms() - t;
        async_ms[k] = a + b;
        async_frame[k] = now_ms() - f;
    }
    d3d8_readback_reset();
    printf("capture calls, median of %d frames, title %dx%d: LockRect %.3f ms, asynchronous %.3f ms\n",
           TIMED, W, H, median(sync_ms, TIMED), median(async_ms, TIMED));
    printf("whole frame (draw, capture, Present), median: no capture %.3f ms, LockRect %.3f ms, asynchronous %.3f ms\n",
           median(none_frame, TIMED), median(sync_frame, TIMED), median(async_frame, TIMED));
}

#ifdef _WIN32
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }
static HWND make_window(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d8_readback_fixture";
    RegisterClassA(&wc);
    /* Never shown: the swap chain needs a window, the test does not need to see it. */
    return CreateWindowExA(0, wc.lpszClassName, "d3d8_readback", WS_OVERLAPPEDWINDOW, 0, 0, W, H,
                           NULL, NULL, wc.hInstance, NULL);
}
#else
static HWND make_window(void) { return NULL; }
#endif

int main(int argc, char **argv)
{
    int scaled = argc > 1 && !strcmp(argv[1], "scaled");
    IDirect3D8 *d3d = xbox_Direct3DCreate8(0);
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3D8Readback rb;
    int k, got = 0, next = 0, started, r;

    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = make_window();
    pp.Windowed = TRUE;

    if (scaled) {
        d3d8_present_enable_scaling(1);
        d3d8_present_set_render_scale(2);
    }
    CHECK(d3d8_readback_begin(1) == -1, "begin before a device must be unsupported");
    if (!d3d || d3d->lpVtbl->CreateDevice(d3d, 0, 1, pp.hDeviceWindow, 0, &pp, &dev) != 0 || !dev) {
        printf("SKIP: no Direct3D 8 device on this machine\n");
        return 77;      /* ctest: skipped, not passed */
    }

    /* Probe: is there an asynchronous path at all? */
    draw_frame(dev, 0);
    started = d3d8_readback_begin(1000);
#if !defined(_WIN32) && !defined(XBOXRECOMP_HAVE_VULKAN)    /* the OpenGL device */
    CHECK(started == -1, "the OpenGL device must report no asynchronous path (got %d)", started);
    printf("%s: unsupported, as expected\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
#endif
    CHECK(started == 1, "begin on a fresh device (got %d)", started);
#if defined(XBOXRECOMP_HAVE_VULKAN)
    CHECK(d3d8_readback_poll(&rb) == 0, "Vulkan: a copy is not done before its frame is submitted");
#endif
    dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
    /* Drain the probe (Direct3D 11 may need a moment). */
    for (r = 0; r < 2000 && !d3d8_readback_poll(&rb); r++) {
#ifdef _WIN32
        Sleep(1);
#endif
    }
    CHECK(r < 2000 && rb.tag == 1000, "the probe copy came back");

    /* A full ring answers 0 until something is collected. */
    for (k = 0; k < D3D8_READBACK_SLOTS; k++)
        CHECK(d3d8_readback_begin(2000 + (uint64_t)k) == 1, "slot %d free", k);
    CHECK(d3d8_readback_begin(2999) == 0, "a full ring answers 0");
    dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
    for (k = 0, r = 0; k < D3D8_READBACK_SLOTS && r < 4000; r++) {
        if (d3d8_readback_poll(&rb)) {
            CHECK(rb.tag == 2000 + (uint64_t)k, "ring order: got %llu, want %d", (unsigned long long)rb.tag, 2000 + k);
            k++;
        } else {
#ifdef _WIN32
            Sleep(1);
#endif
        }
    }
    CHECK(k == D3D8_READBACK_SLOTS, "every ring copy came back (%d)", k);
    CHECK(d3d8_readback_poll(&rb) == 0, "nothing left in flight");

    /* The frames: reference by LockRect, then the asynchronous copy, then Present.
     * Collected one or two frames late, as a recorder would. */
    for (k = 0; k < FRAMES; k++) {
        draw_frame(dev, k);
        CHECK(lock_reference(dev, k), "LockRect reference for frame %d", k);
        r = d3d8_readback_begin((uint64_t)k);
        if (r == 0) {           /* all busy: collect one, then try again */
            if (d3d8_readback_poll(&rb)) {
                CHECK(rb.tag == (uint64_t)next, "order: got %llu, want %d", (unsigned long long)rb.tag, next);
                CHECK(compare(&rb), "frame %d: asynchronous pixels equal LockRect's", (int)rb.tag);
                next++; got++;
            }
            r = d3d8_readback_begin((uint64_t)k);
        }
        CHECK(r == 1, "begin for frame %d (got %d)", k, r);
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        while (d3d8_readback_poll(&rb)) {
            CHECK(rb.tag == (uint64_t)next, "order: got %llu, want %d", (unsigned long long)rb.tag, next);
            CHECK(compare(&rb), "frame %d: asynchronous pixels equal LockRect's", (int)rb.tag);
            next++; got++;
        }
    }
    for (r = 0; got < FRAMES && r < 4000; r++) {
        if (d3d8_readback_poll(&rb)) {
            CHECK(rb.tag == (uint64_t)next, "order: got %llu, want %d", (unsigned long long)rb.tag, next);
            CHECK(compare(&rb), "frame %d: asynchronous pixels equal LockRect's", (int)rb.tag);
            next++; got++;
        } else {
            dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);   /* Vulkan completes with a submit */
        }
    }
    CHECK(got == FRAMES, "all %d frames came back (%d)", FRAMES, got);
    CHECK(!scaled || g_ref_w >= 2 * W, "scaled: the target is at least twice the title's width (%u)", g_ref_w);
    CHECK(g_ref[0] && g_ref[1] && memcmp(g_ref[0], g_ref[1], (size_t)g_ref_w * g_ref_h * 4) != 0,
          "consecutive frames differ, so a wrong frame would be caught");

    d3d8_readback_reset();
    CHECK(d3d8_readback_poll(&rb) == 0, "nothing after reset");
    CHECK(d3d8_readback_begin(5000) == 1, "begin works again after reset");
    d3d8_readback_reset();
    timing(dev);

    printf("%s: %d frames compared at %ux%u%s, %d failure(s)\n", failures ? "FAILED" : "PASSED", got,
           g_ref_w, g_ref_h, scaled ? " (scaled presentation)" : "", failures);
    return failures ? 1 : 0;
}
