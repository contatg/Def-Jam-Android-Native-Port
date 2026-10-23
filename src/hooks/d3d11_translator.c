/*
 * Stand up a graphics device for the push-buffer translator to draw through:
 * Direct3D 11 on Windows, the toolkit's Vulkan device elsewhere. The device,
 * the frame-end hook and the capture are the same on both; only the window
 * differs, and off Windows it lives in src/host_posix.c.
 *
 * Written for Direct3D 11 first, which is why the names say so:
 *
 * This title's Direct3D 8 is statically linked into the XBE, so it was
 * recompiled along with the game and never calls the runtime's own Direct3D.
 * That leaves the runtime's translator, src/nv2a/nv2a_pgraph_d3d11.c, with no
 * device: it renders through xbox_GetD3DDevice(), which only returns one after
 * something has gone through the factory's CreateDevice path.
 *
 * So the host program creates it. Nothing about it is the title's: it is our
 * window, our swap chain, and the title never learns it exists. The title
 * keeps building its own command stream exactly as it did, the executor in
 * src/kernel/nv2a_pb_exec.c keeps decoding it, and each decoded method is
 * handed to the translator, which turns it into draw calls against this
 * device.
 *
 * That is the documented upgrade path away from the executor's software
 * rasteriser, which can only draw flat untextured geometry already in screen
 * space. It also avoids the alternative, which is identifying and replacing
 * every Direct3D 8 entry point in the lifted image by address; this route
 * needs no symbols at all, because the command stream is the interface.
 *
 * Opt-in through RECOMP_PB_D3D11, which the same variable gates in the
 * executor. Without it nothing here runs and the software path is unchanged.
 */

#include "host.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "d3d8_xbox.h"
#include "nv2a_pgraph_d3d11.h"
#include "texture_pack.h"            /* texture_pack_stats: the HD pack's resident bytes for frames.csv */
#include "pc_settings.h"
#include "pc_input.h"
#include "pc_ui.h"
#include "recomp_settings.h"
#include "recorder.h"
#include "d3d8_readback.h"

/* The title sets a 640x480 mode. Matching it keeps the translator's screen
 * space and ours the same, so a vertex the title placed at 320,240 lands in
 * the middle rather than somewhere that needs explaining. */
#define TRANSLATOR_WIDTH   640
#define TRANSLATOR_HEIGHT  480

static void capture_backbuffer(IDirect3DDevice8 *dev);

static int   s_ready;

void d3d11_translator_pump(void);
void d3d11_translator_report(void);

#if defined(_WIN32)

static HWND  s_window;

/* Borderless full screen: the window loses its frame and covers the monitor
 * it is on. No display-mode change, so Alt-Tab is instant and the swap chain
 * simply follows the window's new size (the toolkit's d3d8_present.c).
 *
 * Everything here runs on the window's own thread. A change that starts
 * anywhere else -- the settings file's owner, later an overlay -- is posted to
 * it as WM_APP_FULLSCREEN. */
#define WM_APP_FULLSCREEN  (WM_APP + 1)
#define WM_APP_RESIZE      (WM_APP + 2)     /* wParam, lParam: the client size to give the window */

static int s_fullscreen;
static WINDOWPLACEMENT s_windowed_placement = { sizeof(WINDOWPLACEMENT) };

static void set_fullscreen(HWND h, int on)
{
    if (!!on == s_fullscreen)
        return;
    if (on) {
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        if (!GetWindowPlacement(h, &s_windowed_placement) ||
            !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
            return;
        s_fullscreen = 1;
        SetWindowLongPtrW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        s_fullscreen = 0;
        SetWindowLongPtrW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(h, &s_windowed_placement);
        SetWindowPos(h, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fprintf(stderr, "[TRANS] %s\n", on ? "full screen" : "windowed");
    fflush(stderr);
}

/* The pointer. It hides over the game when it has been still for two seconds
 * (always, in full screen), and in full screen it cannot leave the window, so
 * a click aimed at the game never lands on another monitor and takes the focus
 * away. [input] hide_cursor and confine_cursor turn each off. */
#define CURSOR_TIMER      1
#define CURSOR_IDLE_MS    2000

static ULONGLONG s_mouse_moved;
static int s_cursor_hidden;
static int s_clipped;

static int cursor_hide_wanted(void)
{
    /* Never while the overlay is open: it is a menu, and the pointer is how you use it. */
    return recomp_settings_get("input", "hide_cursor", 1) && !pc_ui_overlay_open();
}

static void update_cursor(HWND h)
{
    POINT p;
    RECT client;
    int focused = GetForegroundWindow() == h && !IsIconic(h);
    int clip = focused && s_fullscreen && recomp_settings_get("input", "confine_cursor", 1);

    if (clip != s_clipped) {
        if (clip) {
            RECT r;
            POINT tl;
            tl.x = tl.y = 0;
            if (GetClientRect(h, &r) && ClientToScreen(h, &tl)) {
                r.left += tl.x; r.right += tl.x; r.top += tl.y; r.bottom += tl.y;
                ClipCursor(&r);
            }
        } else {
            ClipCursor(NULL);
        }
        s_clipped = clip;
        fprintf(stderr, "[TRANS] pointer %s\n", clip ? "confined to the window" : "released");
        fflush(stderr);
    }
    if (!cursor_hide_wanted()) {
        s_cursor_hidden = 0;
        return;
    }
    if (!s_cursor_hidden && focused && GetCursorPos(&p) && ScreenToClient(h, &p) &&
        GetClientRect(h, &client) && PtInRect(&client, p) &&
        GetTickCount64() - s_mouse_moved > CURSOR_IDLE_MS) {
        s_cursor_hidden = 1;
        SetCursor(NULL);
    }
}

/* Remember the size the player dragged the window to. */
static void remember_window_size(HWND h)
{
    RECT r;
    if (s_fullscreen || IsZoomed(h) || IsIconic(h) || !GetClientRect(h, &r))
        return;
    if (r.right > 0 && r.bottom > 0) {
        pc_display_set("window_width", (int)r.right);
        pc_display_set("window_height", (int)r.bottom);
    }
}

static LRESULT CALLBACK translator_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    /* Keys, mouse buttons, the wheel and focus go to the input layer first; it
     * only watches, so the cases below handle them as before. */
    if (pc_ui_window_message(h, msg, wp, lp))
        return 0;
    pc_input_window_message(h, msg, wp, lp);
    switch (msg) {
    case WM_SYSKEYDOWN:
        /* Alt+Enter. Bit 30 is set on auto-repeat: one toggle per press. */
        if (wp == VK_RETURN && (lp & (1 << 29)) && !(lp & (1 << 30))) {
            pc_display_set("fullscreen", !s_fullscreen);
            return 0;
        }
        break;
    case WM_SYSCHAR:
        if (wp == VK_RETURN)
            return 0;   /* no beep for the Alt+Enter just handled */
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11 && !(lp & (1 << 30))) {
            pc_display_set("fullscreen", !s_fullscreen);
            return 0;
        }
        break;
    case WM_APP_FULLSCREEN:
        set_fullscreen(h, (int)wp);
        return 0;
    case WM_APP_RESIZE: {
        /* The window-size setting changed (from the overlay): give a windowed, unmaximised window that size. */
        RECT now, want;
        if (s_fullscreen || IsZoomed(h) || IsIconic(h) || !GetClientRect(h, &now))
            return 0;
        if (now.right == (LONG)wp && now.bottom == (LONG)lp)
            return 0;
        want.left = want.top = 0;
        want.right = (LONG)wp;
        want.bottom = (LONG)lp;
        AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(h, NULL, 0, 0, want.right - want.left, want.bottom - want.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_EXITSIZEMOVE:
        remember_window_size(h);
        break;
    case WM_MOUSEMOVE:
        s_mouse_moved = GetTickCount64();
        if (s_cursor_hidden) {
            s_cursor_hidden = 0;
            SetCursor(LoadCursorW(NULL, IDC_ARROW));
        }
        break;
    case WM_TIMER:
        if (wp == CURSOR_TIMER) {
            update_cursor(h);
            return 0;
        }
        break;
    case WM_SIZE: case WM_MOVE:
        /* The confinement rectangle follows the window. */
        if (s_clipped) {
            ClipCursor(NULL);
            s_clipped = 0;
        }
        update_cursor(h);
        break;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        update_cursor(h);
        break;
    case WM_SETCURSOR:
        /* No pointer over the picture in full screen, or once it has been
         * still for a while. */
        if (LOWORD(lp) == HTCLIENT && cursor_hide_wanted() && (s_fullscreen || s_cursor_hidden)) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 320;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 240;
        return 0;
    case WM_ERASEBKGND:
        return 1;       /* the renderer paints every pixel, bars included */
    }
    /* Closing the window ends the run. Left to DefWindowProc it would destroy
     * the window and leave the title running with nothing to draw into; the
     * log line keeps a deliberate close from reading like a crash. */
    if (msg == WM_CLOSE) {
        pc_input_stop();        /* a pad must not be left vibrating */
        fprintf(stderr, "[TRANS] window closed by the user; exiting\n");
        fflush(stderr);
        fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND make_window(void)
{
    WNDCLASSEXW wc;
    RECT r, work;
    int x, y, w, h;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = translator_wndproc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    /* The shield from the exe's own resources (src/defjam.rc.in, id 1): title bar, taskbar, Alt+Tab. */
    wc.hIcon         = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm       = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                         GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"DefJamRecompTranslator";
    RegisterClassExW(&wc);

    /* The window is the size the settings ask for, no larger than the desktop
     * allows, and centred. The title still renders 640x480 units into its own
     * target; the picture is scaled into whatever this turns out to be. */
    r.left = 0; r.top = 0;
    r.right = pc_display("window_width", TRANSLATOR_WIDTH * 2);
    r.bottom = pc_display("window_height", TRANSLATOR_HEIGHT * 2);
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    w = r.right - r.left;
    h = r.bottom - r.top;
    x = y = CW_USEDEFAULT;
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
        if (w > work.right - work.left) w = work.right - work.left;
        if (h > work.bottom - work.top) h = work.bottom - work.top;
        x = work.left + (work.right - work.left - w) / 2;
        y = work.top + (work.bottom - work.top - h) / 2;
    }

    return CreateWindowExW(0, L"DefJamRecompTranslator",
                           L"Def Jam: Fight for NY", WS_OVERLAPPEDWINDOW,
                           x, y, w, h,
                           NULL, NULL, GetModuleHandleW(NULL), NULL);
}

/* display.window_width or window_height changed, on whichever thread changed it. */
static void window_size_setting_changed(int width, int height)
{
    if (s_window)
        PostMessageW(s_window, WM_APP_RESIZE, (WPARAM)width, (LPARAM)height);
}

/* display.fullscreen changed, on whichever thread changed it. */
static void fullscreen_setting_changed(int fullscreen)
{
    if (s_window)
        PostMessageW(s_window, WM_APP_FULLSCREEN, (WPARAM)fullscreen, 0);
}

/* The window lives on a thread of its own, which creates it and then does
 * nothing but pump its messages.
 *
 * A window's messages go to the thread that created it. This one used to be
 * created on the main thread, which then becomes the title's main thread and
 * never looks at its queue again, while the pump ran on the GPU executor's
 * thread -- where PeekMessage sees only that thread's own windows. Nothing
 * ever serviced the window. That went unnoticed until something needed an
 * answer from it: a click, focus leaving or returning, Alt-Tab. Five seconds
 * later Windows marked it Not Responding and painted a frozen ghost over it
 * for good. Direct3D presents from the executor thread as before; a swap
 * chain does not need its window's thread, only for that thread to answer. */
static HANDLE s_window_ready;

static DWORD WINAPI window_thread(LPVOID unused)
{
    MSG msg;
    (void)unused;
    /* Real pixels on a scaled desktop: without this Windows renders the
     * window small and stretches it, which blurs the picture and makes "full
     * screen" less than the display's resolution. Windows 10 1703 and later;
     * absent, the window is merely scaled. */
    {
        typedef BOOL (WINAPI *SetDpiContext)(HANDLE);
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        SetDpiContext set = user32
            ? (SetDpiContext)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext") : NULL;
        if (set)
            set((HANDLE)(INT_PTR)-4);   /* PER_MONITOR_AWARE_V2 */
    }
    s_window = make_window();
    if (s_window) {
        ShowWindow(s_window, SW_SHOW);
        s_mouse_moved = GetTickCount64();
        SetTimer(s_window, CURSOR_TIMER, 250, NULL);
        pc_input_set_focus(GetFocus() == s_window);
        pc_settings_on_fullscreen(fullscreen_setting_changed);
        pc_settings_on_window_size(window_size_setting_changed);
        pc_ui_set_window(s_window);
        if (pc_display("fullscreen", 0))
            set_fullscreen(s_window, 1);
    }
    SetEvent(s_window_ready);
    if (!s_window)
        return 1;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

#else

int host_posix_open_window(int width, int height, const char *title);

#endif /* _WIN32 */

int d3d11_translator_init(void)
{
    IDirect3D8 *d3d;
    IDirect3DDevice8 *dev = NULL;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;

    if (s_ready)
        return 1;
    if (!getenv("RECOMP_PB_D3D11"))
        return 0;

#if defined(_WIN32)
    s_window_ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    {
        HANDLE th = s_window_ready ? CreateThread(NULL, 0, window_thread, NULL, 0, NULL) : NULL;
        if (!th) {
            fprintf(stderr, "[TRANS] could not start the window thread (error %lu)\n",
                    GetLastError());
            return 0;
        }
        CloseHandle(th);
    }
    WaitForSingleObject(s_window_ready, INFINITE);
    if (!s_window) {
        fprintf(stderr, "[TRANS] could not create a window\n");
        return 0;
    }
#else
    /* No window is not fatal here: the device then draws off screen, which
     * is what a run without a display uses. */
    if (!host_posix_open_window(pc_display("window_width", TRANSLATOR_WIDTH * 2),
                                pc_display("window_height", TRANSLATOR_HEIGHT * 2),
                                "Def Jam: Fight for NY"))
        fprintf(stderr, "[TRANS] no window; frames are drawn off screen\n");
#endif

    d3d = xbox_Direct3DCreate8(0);
    if (!d3d) {
        fprintf(stderr, "[TRANS] Direct3DCreate8 returned nothing\n");
        return 0;
    }

    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth        = TRANSLATOR_WIDTH;
    pp.BackBufferHeight       = TRANSLATOR_HEIGHT;
    pp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    pp.BackBufferCount        = 1;
    pp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
#if defined(_WIN32)
    pp.hDeviceWindow          = s_window;
#endif
    pp.Windowed               = TRUE;
    pp.EnableAutoDepthStencil = FALSE;

    hr = d3d->lpVtbl->CreateDevice(d3d, 0, 1 /* HAL */,
#if defined(_WIN32)
                                   s_window,
#else
                                   NULL,
#endif
                                   0x00000040 /* SOFTWARE_VERTEXPROCESSING */,
                                   &pp, &dev);
    if (FAILED(hr) || !dev) {
        fprintf(stderr, "[TRANS] CreateDevice failed (0x%08lX)\n", (unsigned long)hr);
        return 0;
    }

    pgraph_d3d11_init();
    s_ready = 1;
    fprintf(stderr, "[TRANS] translator ready: %ux%u picture in a %dx%d window, "
                    "push-buffer methods will be forwarded\n",
            TRANSLATOR_WIDTH, TRANSLATOR_HEIGHT,
            pc_display("window_width", 0), pc_display("window_height", 0));
    fflush(stderr);
    return 1;
}

/* The play-test recorder's share of a frame (recorder.c): a picture for its
 * ring when it asks for one, and the frame's measurements. Nothing runs unless
 * [debug] record is on.
 *
 * The picture is read back asynchronously where the device can
 * (tools/xboxrecomp/src/d3d/d3d8_readback.h: Direct3D 11 and Vulkan): the copy
 * starts here and is collected a frame or two later, filed under the frame that
 * asked (recorder_picture_tagged). Where it cannot (OpenGL, a multisampled
 * target, every slot busy) the picture is read back at once, the way
 * capture_backbuffer reads it. RECOMP_RECORD_SYNC=1 always reads at once, to
 * compare the two costs (capture_ms in frames.csv). */
static void recorder_picture_now(IDirect3DDevice8 *dev, uint64_t tag, const uint8_t (*gamma)[256])
{
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    extern unsigned d3d8_GetRenderScale(void);
    extern float d3d8_GetCanvasScaleX(void);
    uint32_t sw = (uint32_t)(TRANSLATOR_WIDTH * d3d8_GetCanvasScaleX() + 0.5f);
    uint32_t sh = TRANSLATOR_HEIGHT * d3d8_GetRenderScale();

    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, &surf)) || !surf)
        return;
    memset(&lr, 0, sizeof(lr));
    if (SUCCEEDED(surf->lpVtbl->LockRect(surf, &lr, NULL, D3DLOCK_READONLY)) && lr.pBits) {
        if (tag)
            recorder_picture_tagged(tag, (const uint8_t *)lr.pBits, lr.Pitch, (int)sw, (int)sh, 0, gamma);
        else
            recorder_picture((const uint8_t *)lr.pBits, lr.Pitch, (int)sw, (int)sh, gamma);
        surf->lpVtbl->UnlockRect(surf);
    }
    /* Direct3D 11's GetBackBuffer makes a new surface, holding the texture, every call. */
    surf->lpVtbl->Release(surf);
}

static double recorder_capture(IDirect3DDevice8 *dev)
{
    static int async = -1;          /* -1 not decided, 0 read at once, 1 asynchronous */
    extern uint8_t g_recomp_gamma[3][256];
    extern int g_recomp_gamma_on;
    const uint8_t (*gamma)[256] = g_recomp_gamma_on ? (const uint8_t (*)[256])g_recomp_gamma : NULL;
    D3D8Readback rb;
    double start = 0.0;
    int polled = 0;

    if (async < 0) {
        const char *e = getenv("RECOMP_RECORD_SYNC");
        async = (e && *e == '1') ? 0 : 1;
    }
    if (async) {
        start = recorder_now_ms();
        /* The pictures asked for a frame or two ago. */
        while (d3d8_readback_poll(&rb)) {
            recorder_picture_tagged(rb.tag, rb.pixels, rb.pitch, (int)rb.width, (int)rb.height, rb.bgra, gamma);
            polled = 1;
        }
    }
    if (!recorder_picture_wanted())
        return polled ? recorder_now_ms() - start : 0.0;
    if (!polled)
        start = recorder_now_ms();
    if (async) {
        uint64_t tag = recorder_picture_request();
        int r = tag ? d3d8_readback_begin(tag) : 1;
        if (r < 0) {
            async = 0;              /* this device has no asynchronous path: stop asking */
            fprintf(stderr, "[RECORD] pictures are read back synchronously on this device\n");
        }
        if (r <= 0)
            recorder_picture_now(dev, tag, gamma);
    } else {
        recorder_picture_now(dev, 0, gamma);
    }
    return recorder_now_ms() - start;
}

static void recorder_frame_done(double present_ms, double capture_ms)
{
    extern int g_xa2_dropped, g_xa2_starved;
    extern long long defjam_test_current_step(void);
    PgraphD3D11Stats st;
    RecorderFrameIn in;

    memset(&in, 0, sizeof in);
    pgraph_d3d11_get_stats(&st);
    in.present_ms = present_ms;
    in.capture_ms = capture_ms;
    in.draws = st.draw_calls;
    in.vertices = st.vertices_submitted;
    in.clears = st.clears;
    in.audio_dropped = g_xa2_dropped;
    in.audio_dry = g_xa2_starved;
    in.step = defjam_test_current_step();
    in.tex_uploads = st.textures_uploaded;
    in.tex_evictions = st.texture_evictions;
    in.tex_upload_bytes = st.texture_upload_bytes;
    in.tex_cached = st.textures_cached;
    in.tex_cache_bytes = st.texture_cache_bytes;
    {
        uint64_t resident = 0;
        texture_pack_stats(&resident, NULL);
        in.hd_cache_bytes = resident;
    }
    /* Where the log has got to: with this in every row, scripts/bug-bundle.py puts each
     * log line on the frames' clock without the log's format changing. Flushing once a
     * frame also keeps the log's end on disk if the process is killed. */
    fflush(stderr);
    in.log_bytes = (long long)ftell(stderr);
    recorder_frame_end(&in);
}

/* Put the frame on screen.
 *
 * pgraph_d3d11_flush() submits whatever geometry was still batched and counts
 * the frame, but it does not present: the translator draws into a device it
 * did not create and does not own the swap chain. So the host presents, which
 * is also where the window has to be pumped and the counters read. Without
 * this every frame is rendered correctly and then discarded, which looks
 * exactly like a renderer that does nothing. */
void d3d11_translator_frame_end(void)
{
    IDirect3DDevice8 *dev;

    if (!s_ready)
        return;
    dev = xbox_GetD3DDevice();
    if (dev) {
        double capture_ms = 0.0, present_at = 0.0;
        int recording = recorder_on();
        capture_backbuffer(dev);
        if (recording) {
            recorder_frame_begin();
            capture_ms = recorder_capture(dev);
            present_at = recorder_now_ms();
        }
        dev->lpVtbl->Present(dev, NULL, NULL, NULL, NULL);
        if (recording)
            recorder_frame_done(recorder_now_ms() - present_at, capture_ms);
        /* While the display paces the presents (display.vsync on a 60, 120,
         * 180 or 240 Hz display) it is the frame clock, and the executor's
         * 60 Hz flip timer would drift against it. Loosen the timer to a
         * ceiling just above 60 instead of removing it: a minimised or covered
         * window presents at once, and the title must not run away then. */
        {
            extern int d3d8_present_display_paced(void);
            extern void xbox_Nv2aSetFlipHz(int hz);
            xbox_Nv2aSetFlipHz(d3d8_present_display_paced() ? 63 : 0);
        }
    }
    d3d11_translator_pump();
    d3d11_translator_report();
}

/* Save what was actually rendered, straight out of the back buffer.
 *
 * Needed because the alternative is screenshotting the desktop, which catches
 * whatever else is on it. This reads only our own render target, so it is
 * bounded to the thing being tested, and it is also what the golden-image
 * comparison in docs/02-test-plan.md needs.
 *
 * Writes a 24-bit BMP, which needs no library and which anything opens. Set
 * RECOMP_TRANS_SHOT=<path> and the next presented frame lands there. */
static void capture_backbuffer(IDirect3DDevice8 *dev)
{
    const char *path = getenv("RECOMP_TRANS_SHOT");
    IDirect3DSurface8 *surf = NULL;
    D3DLOCKED_RECT lr;
    FILE *f;
    uint32_t w = TRANSLATOR_WIDTH, h = TRANSLATOR_HEIGHT;
    uint32_t row = ((w * 3) + 3) & ~3u;
    uint8_t hdr[54];
    uint32_t y, x, size;
    extern unsigned d3d8_GetRenderScale(void);
    extern float d3d8_GetCanvasScaleX(void);
    uint32_t sc = d3d8_GetRenderScale();
    /* The target's size: the title's times the render scale, wider across when
     * it is a widescreen canvas (docs/widescreen/02-design.md). */
    uint32_t sw = (uint32_t)(TRANSLATOR_WIDTH * d3d8_GetCanvasScaleX() + 0.5f);
    uint32_t sh = TRANSLATOR_HEIGHT * sc;
    const char *native_shot = getenv("RECOMP_TRANS_SHOT_NATIVE");
    const char *wide_shot = getenv("RECOMP_TRANS_SHOT_WIDE");
    /* HD review needs actual rendered pixels; regression captures stay 640x480
     * (a widescreen canvas squeezed to the title's coordinates); WIDE keeps the
     * canvas's shape at the title's height. */
    if (native_shot && !strcmp(native_shot,"1")) {
        w = sw; h = sh;
    } else if (wide_shot && !strcmp(wide_shot, "1")) {
        w = (sw + sc / 2) / sc;
    }
    row = ((w * 3) + 3) & ~3u;

    char named[512];
    /* The frames the caller asked for, ascending. One frame writes to the path
     * as given; a list ("1300,1900,2600") writes one file per frame with the
     * frame number before the extension, so a single run shows when the
     * picture changes. The first few frames are cleared and nothing else,
     * because the title is still loading, so frame zero says nothing. */
    static uint32_t want[16];
    static int nwant = -1, next;
    static uint32_t seen;
    /* Or by time: RECOMP_TRANS_SHOT_SECS=150,165.5 takes the first frame
     * presented after each point, in seconds since process start (or since
     * RECOMP_SCRIPT_ANCHOR's file open) -- the clock
     * RECOMP_PAD_SCRIPT uses. Frame numbers drift by hundreds between runs, so
     * a frame list cannot be aimed at the screen a scripted press leads to.
     * Files are named <stem>-<secs>s<ext>. An "@file#N" entry puts the times
     * after it on xbox_FileOpenSeconds's clock instead, as in RECOMP_PAD_SCRIPT:
     * RECOMP_TRANS_SHOT_SECS=20,@main.mus#1,3,10. */
    static double want_secs[16];
    static char want_base[16][64];
    static int by_secs;
    double now_secs = 0.0;
    long step_shot = 0;

    /* A capture asked for at a fight step (RECOMP_TEST_SHOT, test_telemetry.c):
     * with step-timed input and pinned seeds the game's state at a step is the
     * same every run, so this frame can be compared with a baseline. It takes
     * precedence over, and does not consume, the frame and time lists below. */
    {
        extern long defjam_test_shot_due(void);
        const char *stem = getenv("RECOMP_TEST_SHOT");
        step_shot = (stem && *stem) ? defjam_test_shot_due() : 0;
        if (step_shot) {
            const char *dot = strrchr(stem, '.');
            int n = dot ? (int)(dot - stem) : (int)strlen(stem);
            snprintf(named, sizeof(named), "%.*s-step%ld%s", n, stem, step_shot, dot ? dot : "");
            path = named;
        }
    }
    if (!step_shot) {
    if (!path || !*path)
        return;
    if (nwant < 0) {
        const char *s = getenv("RECOMP_TRANS_SHOT_FRAME");
        const char *t = getenv("RECOMP_TRANS_SHOT_SECS");
        nwant = 0;
        if (t && *t) {
            by_secs = 1;
            s = t;
        }
        while (s && *s && nwant < 16) {
            char *end;
            if (by_secs && *s == '@') {
                size_t n = strcspn(s + 1, ",");
                char base[64];
                int k;
                snprintf(base, sizeof base, "%.*s", (int)n, s + 1);
                for (k = nwant; k < 16; k++)
                    strcpy(want_base[k], base);
                {
                    /* A watch starts at its first query: make it now. */
                    extern double xbox_FileOpenSeconds(const char *spec);
                    (void)xbox_FileOpenSeconds(base);
                }
                s += 1 + n;
                s = (*s == ',') ? s + 1 : "";
                continue;
            }
            if (by_secs)
                want_secs[nwant++] = strtod(s, &end);
            else
                want[nwant++] = (uint32_t)strtoul(s, &end, 0);
            s = (*end == ',') ? end + 1 : "";
        }
    }
    if (by_secs) {
        extern double xbox_ScriptSeconds(void);   /* honours RECOMP_SCRIPT_ANCHOR */
        extern double xbox_FileOpenSeconds(const char *spec);
        if (next >= nwant)
            return;
        now_secs = want_base[next][0] ? xbox_FileOpenSeconds(want_base[next])
                                      : xbox_ScriptSeconds();
        if (now_secs < 0.0 || now_secs < want_secs[next])
            return;
    } else if (nwant > 0 && (next >= nwant || seen++ < want[next])) {
        return;
    }
    if (nwant > 1 || by_secs) {
        const char *dot = strrchr(path, '.');
        int stem = dot ? (int)(dot - path) : (int)strlen(path);
        if (by_secs)
            snprintf(named, sizeof(named), "%.*s-%gs%s", stem, path, want_secs[next],
                     dot ? dot : "");
        else
            snprintf(named, sizeof(named), "%.*s-%u%s", stem, path, seen - 1,
                     dot ? dot : "");
        path = named;
    }
    }   /* !step_shot */
    if (FAILED(dev->lpVtbl->GetBackBuffer(dev, 0, 0, &surf)) || !surf)
        return;
    memset(&lr, 0, sizeof(lr));
    if (FAILED(surf->lpVtbl->LockRect(surf, &lr, NULL, 0)) || !lr.pBits) {
        fprintf(stderr, "[TRANS] could not lock the back buffer\n");
        return;
    }

    f = fopen(path, "wb");
    if (f) {
        size = 54 + row * h;
        memset(hdr, 0, sizeof(hdr));
        hdr[0] = 'B'; hdr[1] = 'M';
        hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8);
        hdr[4] = (uint8_t)(size >> 16); hdr[5] = (uint8_t)(size >> 24);
        hdr[10] = 54; hdr[14] = 40;
        hdr[18] = (uint8_t)w; hdr[19] = (uint8_t)(w >> 8);
        hdr[22] = (uint8_t)h; hdr[23] = (uint8_t)(h >> 8);
        hdr[26] = 1; hdr[28] = 24;
        fwrite(hdr, 1, sizeof(hdr), f);
        /* BMP rows run bottom to top. At a render scale above 1 the back
         * buffer is that many times the title's size, and each pixel written
         * is the mean of the block of target pixels it covers -- what the
         * window shows. (sc x sc for the usual 4:3 target.) */
        for (y = 0; y < h; y++) {
            uint32_t written = 0;
            uint32_t sy0 = (uint32_t)(((uint64_t)(h - 1 - y) * sh) / h);
            uint32_t sy1 = (uint32_t)(((uint64_t)(h - y) * sh) / h);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            /* The runtime's swap chain is R8G8B8A8 (d3d8_device.c), so the
             * locked bytes run R, G, B, A and a BMP wants B, G, R. Written in
             * the locked order, every capture had red and blue swapped -- skin
             * blue, a blue jersey orange, the Xbox B button blue -- while the
             * window was right. */
            for (x = 0; x < w; x++) {
                uint32_t sum[3] = { 0, 0, 0 }, i, j, c;
                uint32_t sx0 = (uint32_t)(((uint64_t)x * sw) / w);
                uint32_t sx1 = (uint32_t)(((uint64_t)(x + 1) * sw) / w);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                for (j = sy0; j < sy1; j++) {
                    const uint8_t *src = (const uint8_t *)lr.pBits + (size_t)j * lr.Pitch;
                    for (i = sx0; i < sx1; i++)
                        for (c = 0; c < 3; c++)
                            sum[c] += src[i * 4 + c];
                }
                for (c = 0; c < 3; c++) {
                    /* The title's gamma ramp is applied at presentation,
                     * after this read; apply it here so a capture is what
                     * the window shows (recomp_manual.c). */
                    extern uint8_t g_recomp_gamma[3][256];
                    extern int g_recomp_gamma_on;
                    sum[c] /= (sx1 - sx0) * (sy1 - sy0);
                    if (g_recomp_gamma_on)
                        sum[c] = g_recomp_gamma[c][sum[c] & 0xFF];
                }
                fputc((int)sum[2], f);   /* B */
                fputc((int)sum[1], f);   /* G */
                fputc((int)sum[0], f);   /* R */
                written += 3;
            }
            while (written++ < row)
                fputc(0, f);
        }
        fclose(f);
        fprintf(stderr, "[TRANS] captured the back buffer to %s\n", path);
        {
            /* And, with RECOMP_PB_BATCH_DUMP=shot, the next frame's batches. */
            extern void nv2a_pb_request_dump_next(void);
            nv2a_pb_request_dump_next();
        }
        if (!step_shot && ++next >= nwant)
            _putenv_s("RECOMP_TRANS_SHOT", "");   /* every requested frame taken */
    }
    surf->lpVtbl->UnlockRect(surf);
    fflush(stderr);
}

/* Kept for its callers: the window's own thread pumps it now (window_thread
 * above), and a PeekMessage here, on the executor's thread, could only ever
 * see that thread's windows. */
void d3d11_translator_pump(void)
{
}

/* What the translator actually did, which is the only way to tell "no methods
 * arrived" from "methods arrived and were all ignored" from "it drew and the
 * result is off screen". Called on a timer from the host so a run reports
 * without needing the title to exit. */
void d3d11_translator_report(void)
{
    static PgraphD3D11Stats prev;
    static DWORD last_report;
    PgraphD3D11Stats st;
    DWORD now;

    if (!s_ready)
        return;
    now = GetTickCount();
    if (last_report && now - last_report < 2000)
        return;
    last_report = now;
    pgraph_d3d11_get_stats(&st);
    if (memcmp(&st, &prev, sizeof(st)) == 0)
        return;
    prev = st;
    fprintf(stderr, "[TRANS] frames %u  draws %u  vertices %u  "
                    "methods handled %u ignored %u  clears %u\n",
            st.frames, st.draw_calls, st.vertices_submitted,
            st.methods_handled, st.methods_ignored, st.clears);
    fflush(stderr);
}
