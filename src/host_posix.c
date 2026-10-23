/*
 * host_posix.c -- the window, and the Windows-only host files' stand-ins, for
 * hosts that are not Windows.
 *
 * The window is SDL3's. On macOS a window's events can only be read on the
 * process's first thread, so that thread is kept for them: the game boots and
 * runs on a second thread (host_posix_run), and anything that has to touch
 * the window is handed to the first one (call_on_main). On Linux the same
 * arrangement is simply harmless.
 *
 * The toolkit's Vulkan device is told how to make a surface for the window
 * (d3d8_vk.h); it does the rest. With RECOMP_HEADLESS set, or when SDL cannot
 * open a display, there is no window and the game runs on the first thread:
 * frames are still drawn off screen and can be captured, which is all an
 * unattended run needs.
 *
 * src/hooks/watchpoint.c drives the x86 debug registers through the Win32
 * thread API and has no counterpart here.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#ifdef __APPLE__
#include <ApplicationServices/ApplicationServices.h>
#endif

#include "app_icon_rgba.h"   /* generated from branding/icon/window (cmake/app_icon.cmake) */
#include "d3d8_vk.h"
#include "hooks/pc_settings.h"
#include "hooks/pc_input.h"
#include "hooks/pc_ui.h"

static SDL_Window *s_window;
static int         s_have_video;
static int         s_fullscreen;
static volatile int s_drawable_w, s_drawable_h;
static volatile unsigned s_refresh_hz;   /* the window's display, 0 if unknown */
static Uint32      s_call_event;
static const char *const *s_extensions;
static Uint32      s_extension_count;
#if defined(__ANDROID__)
static const char *const s_android_extensions[] = {
    "VK_KHR_surface",
    "VK_KHR_android_surface"
};
#endif

/* ── Work for the first thread ─────────────────────────────────────────── */

typedef struct {
    void (*fn)(void *);
    void *arg;
    pthread_mutex_t lock;
    pthread_cond_t  done_cond;
    int done;
} MainCall;

/* Run fn(arg) on the first thread and wait for it. */
static void call_on_main(void (*fn)(void *), void *arg)
{
    MainCall c;
    SDL_Event ev;

    c.fn = fn;
    c.arg = arg;
    c.done = 0;
    pthread_mutex_init(&c.lock, NULL);
    pthread_cond_init(&c.done_cond, NULL);
    SDL_zero(ev);
    ev.type = s_call_event;
    ev.user.data1 = &c;
    SDL_PushEvent(&ev);
    pthread_mutex_lock(&c.lock);
    while (!c.done)
        pthread_cond_wait(&c.done_cond, &c.lock);
    pthread_mutex_unlock(&c.lock);
    pthread_cond_destroy(&c.done_cond);
    pthread_mutex_destroy(&c.lock);
}

static void refresh_drawable(void)
{
    int w = 0, h = 0;
    unsigned hz = 0;
    if (s_window) {
        const SDL_DisplayMode *mode;
        SDL_GetWindowSizeInPixels(s_window, &w, &h);
        mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(s_window));
        if (mode && mode->refresh_rate > 0.0f)
            hz = (unsigned)(mode->refresh_rate + 0.5f);
    }
    s_drawable_w = w;
    s_drawable_h = h;
    if (hz != s_refresh_hz) {
        fprintf(stderr, "[HOST] display refresh %u Hz\n", hz);
        fflush(stderr);
    }
    s_refresh_hz = hz;
}

/* For the launcher (src/hooks/pc_launcher_sdl.cpp), which has a window of its own on this thread. */
void host_posix_call_on_main(void (*fn)(void *), void *arg) { call_on_main(fn, arg); }
int  host_posix_have_video(void) { return s_have_video; }

/* The primary display's size, for widescreen's "auto" (src/patches/widescreen.c). Asked on
 * the main thread, where SDL's video calls belong. 0 when there is no display. */
typedef struct { int w, h; } DisplaySize;
static void display_size_main(void *arg)
{
    DisplaySize *d = arg;
#ifdef __APPLE__
    /* Fullscreen Cocoa content excludes the camera/menu strip on notched
     * screens. Use that screen shape in windowed mode too: the canvas is fixed
     * for the run, so entering fullscreen later must keep the same framing. */
    extern int host_macos_display_size(int *w, int *h);
    if (host_macos_display_size(&d->w, &d->h))
        return;
#endif
    const SDL_DisplayMode *m = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    if (m) {
        d->w = m->w;
        d->h = m->h;
    }
}
int host_posix_display_size(int *w, int *h)
{
    DisplaySize d = { 0, 0 };
    if (!s_have_video)
        return 0;
    call_on_main(display_size_main, &d);
    if (d.w <= 0 || d.h <= 0)
        return 0;
    *w = d.w;
    *h = d.h;
    return 1;
}

/* Whether Shift is down right now, before any key event has said so: the launcher is shown when
 * it is held at start-up. */
int host_posix_shift_held(void)
{
#ifdef __APPLE__
    return (CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState) & kCGEventFlagMaskShift) != 0;
#else
    return s_have_video && (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
}

/* ── The window ────────────────────────────────────────────────────────── */

typedef struct { int w, h; const char *title; } WindowRequest;

static void make_window_main(void *arg)
{
    WindowRequest *r = arg;

    s_window = SDL_CreateWindow(r->title, r->w, r->h,
                                SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE
                                | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!s_window) {
        fprintf(stderr, "[HOST] no window: %s\n", SDL_GetError());
        return;
    }
    SDL_SetWindowMinimumSize(s_window, 320, 240);
    {   /* The shield: the window's icon, and on macOS the Dock's. The Linux taskbar and
         * switcher use the desktop entry's icon; this covers a run outside it. */
        SDL_Surface *icon = SDL_CreateSurfaceFrom(APP_ICON_SIZE, APP_ICON_SIZE, SDL_PIXELFORMAT_RGBA32,
                                                  (void *)app_icon_rgba, APP_ICON_SIZE * 4);
        if (icon) {
            SDL_SetWindowIcon(s_window, icon);
            SDL_DestroySurface(icon);
        }
    }
#if defined(__ANDROID__)
    /* Do not ask SDL's system Vulkan loader for these when a custom Turnip
     * loader may be active. The required Android WSI extensions are fixed. */
    s_extensions = s_android_extensions;
    s_extension_count = (Uint32)(sizeof s_android_extensions / sizeof s_android_extensions[0]);
#else
    s_extensions = SDL_Vulkan_GetInstanceExtensions(&s_extension_count);
    if (!s_extensions) {
        fprintf(stderr, "[HOST] SDL cannot name its Vulkan extensions: %s\n", SDL_GetError());
        SDL_DestroyWindow(s_window);
        s_window = NULL;
        return;
    }
#endif
    refresh_drawable();
}

typedef struct { void *instance; void *surface; int ok; } SurfaceRequest;

static void make_surface_main(void *arg)
{
    SurfaceRequest *r = arg;
#if defined(__ANDROID__)
    void *native_window = SDL_GetPointerProperty(SDL_GetWindowProperties(s_window),
                                                  SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
    r->ok = d3d8_vk_android_create_surface(r->instance, native_window, r->surface);
#else
    r->ok = SDL_Vulkan_CreateSurface(s_window, (VkInstance)r->instance, NULL,
                                     (VkSurfaceKHR *)r->surface);
#endif
    if (!r->ok)
        fprintf(stderr, "[HOST] no Vulkan surface: %s\n", SDL_GetError());
    refresh_drawable();
}

static int create_surface(void *instance, void *surface, void *user)
{
    SurfaceRequest r = { instance, surface, 0 };
    (void)user;
    call_on_main(make_surface_main, &r);
    return r.ok;
}

static void drawable_size(int *w, int *h, void *user)
{
    (void)user;
    *w = s_drawable_w;
    *h = s_drawable_h;
}

/* Read on the first thread with the size (refresh_drawable); the Vulkan
 * device only uses vsync on a display refreshing at a multiple of 60. */
static unsigned refresh_hz(void *user)
{
    (void)user;
    return s_refresh_hz;
}

static void set_fullscreen_main(void *arg)
{
    int on = arg != NULL;
    if (!s_window || on == s_fullscreen)
        return;
    s_fullscreen = on;
    SDL_SetWindowFullscreen(s_window, on != 0);
    if (on) SDL_HideCursor(); else SDL_ShowCursor();
    refresh_drawable();
    fprintf(stderr, "[TRANS] %s\n", on ? "full screen" : "windowed");
    fflush(stderr);
}

/* display.fullscreen changed, on whichever thread changed it. */
static void fullscreen_setting_changed(int fullscreen)
{
    SDL_Event ev;
    static MainCall unused;
    (void)unused;
    /* Not call_on_main: this can arrive on the first thread itself (a hotkey),
     * which must not wait for itself. */
    SDL_zero(ev);
    ev.type = s_call_event + 1;
    ev.user.code = fullscreen;
    SDL_PushEvent(&ev);
}

/* The overlay opened or closed, on the thread that draws. The pointer must show while it is up
 * (full screen hides it), and typing into its text boxes must be delivered. */
void host_posix_overlay_changed(int open)
{
    SDL_Event ev;
    if (!s_have_video || !s_window)
        return;
    SDL_zero(ev);
    ev.type = s_call_event + 3;
    ev.user.code = open;
    SDL_PushEvent(&ev);
}

/* display.window_width or window_height changed (the overlay's boxes, or the window itself being
 * resized, which writes them): give a windowed, unmaximised window that size. */
static void window_size_main(int w, int h)
{
    int cw = 0, ch = 0;

    if (!s_window || s_fullscreen || w <= 0 || h <= 0)
        return;
    if (SDL_GetWindowFlags(s_window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))
        return;
    SDL_GetWindowSize(s_window, &cw, &ch);
    if (cw == w && ch == h)
        return;
    SDL_SetWindowSize(s_window, w, h);
    refresh_drawable();
    fprintf(stderr, "[TRANS] window size set to %dx%d (the setting changed)\n", w, h);
    fflush(stderr);
}

/* The size settings changed, on whichever thread changed them. */
static void window_size_setting_changed(int width, int height)
{
    SDL_Event ev;

    if (!s_have_video || !s_window)
        return;
    SDL_zero(ev);
    ev.type = s_call_event + 4;
    ev.user.code = width;
    ev.user.data1 = (void *)(intptr_t)height;
    SDL_PushEvent(&ev);
}

static void overlay_changed_main(int open)
{
    if (!s_window)
        return;
    if (open) {
        SDL_ShowCursor();
        SDL_StartTextInput(s_window);
    } else {
        SDL_StopTextInput(s_window);
        if (s_fullscreen)
            SDL_HideCursor();
    }
}

/* Open the window the picture goes to and tell the graphics device about it.
 * Returns 0 when there is none; the device then draws off screen. */
int host_posix_open_window(int width, int height, const char *title)
{
    static D3D8VkHost host;
    WindowRequest r = { width, height, title };

    if (!s_have_video)
        return 0;
#if defined(__ANDROID__)
    // Finger events are handled directly by the overlay; avoid duplicate mouse clicks.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    call_on_main(make_window_main, &r);
    if (!s_window)
        return 0;
    {
        int pw = 0, ph = 0, w = 0, h = 0;
        SDL_GetWindowSize(s_window, &w, &h);
        SDL_GetWindowSizeInPixels(s_window, &pw, &ph);
        fprintf(stderr, "[HOST] window %dx%d points, %dx%d pixels\n", w, h, pw, ph);
    }
    host.instance_extensions = s_extensions;
    host.instance_extension_count = s_extension_count;
    host.create_surface = create_surface;
    host.drawable_size = drawable_size;
    host.refresh_hz = refresh_hz;
    d3d8_vk_set_host(&host);
    pc_settings_on_fullscreen(fullscreen_setting_changed);
    pc_settings_on_window_size(window_size_setting_changed);
    if (pc_display("fullscreen", 0))
        fullscreen_setting_changed(1);
    return 1;
}

/* ── The first thread ──────────────────────────────────────────────────── */

static int (*s_game_main)(void);
static volatile int s_game_done, s_game_result;

static void *game_thread(void *unused)
{
    (void)unused;
    s_game_result = s_game_main();
    s_game_done = 1;
    {
        SDL_Event ev;
        SDL_zero(ev);
        ev.type = s_call_event + 2;
        SDL_PushEvent(&ev);
    }
    return NULL;
}

/* A key by where it is on the keyboard, as the Windows virtual-key code the
 * input layer's bindings are written in; 0 for a key it has no name for.
 * Shift, Ctrl and Alt come by side (VK_LSHIFT and so on). */
int host_posix_vk_from_scancode(int scancode)
{
    SDL_Scancode sc = (SDL_Scancode)scancode;

    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)        return 'A' + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9)        return '1' + (sc - SDL_SCANCODE_1);
    if (sc == SDL_SCANCODE_0)                                return '0';
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12)     return 0x70 + (sc - SDL_SCANCODE_F1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9)  return 0x61 + (sc - SDL_SCANCODE_KP_1);
    switch (sc) {
    case SDL_SCANCODE_KP_0:      return 0x60;
    case SDL_SCANCODE_RETURN:    return 0x0D;
    case SDL_SCANCODE_KP_ENTER:  return 0x0D;
    case SDL_SCANCODE_ESCAPE:    return 0x1B;
    case SDL_SCANCODE_BACKSPACE: return 0x08;
    case SDL_SCANCODE_TAB:       return 0x09;
    case SDL_SCANCODE_SPACE:     return 0x20;
    case SDL_SCANCODE_LEFT:      return 0x25;
    case SDL_SCANCODE_UP:        return 0x26;
    case SDL_SCANCODE_RIGHT:     return 0x27;
    case SDL_SCANCODE_DOWN:      return 0x28;
    case SDL_SCANCODE_LSHIFT:    return 0xA0;
    case SDL_SCANCODE_RSHIFT:    return 0xA1;
    case SDL_SCANCODE_LCTRL:     return 0xA2;
    case SDL_SCANCODE_RCTRL:     return 0xA3;
    case SDL_SCANCODE_LALT:      return 0xA4;
    case SDL_SCANCODE_RALT:      return 0xA5;
    default:                     return 0;
    }
}

/* A key to the input layer. Shift, Ctrl and Alt are reported by side and as
 * the generic key, held while either side is, as the Win32 window reports them. */
static void key_to_input(SDL_Scancode sc, int down)
{
    static unsigned char side[3][2];
    int vk = host_posix_vk_from_scancode((int)sc);

    if (vk >= 0xA0 && vk <= 0xA5) {
        static const int generic[3] = { 0x10, 0x11, 0x12 };          /* VK_SHIFT, CONTROL, MENU */
        int mod = (vk - 0xA0) / 2, right = (vk - 0xA0) % 2;
        side[mod][right] = (unsigned char)down;
        pc_input_host_key(vk, down);
        pc_input_host_key(generic[mod], side[mod][0] || side[mod][1]);
        return;
    }
    if (vk)
        pc_input_host_key(vk, down);
}

static void handle_event(const SDL_Event *ev)
{
    if (ev->type == s_call_event) {
        MainCall *c = ev->user.data1;
        c->fn(c->arg);
        pthread_mutex_lock(&c->lock);
        c->done = 1;
        pthread_cond_signal(&c->done_cond);
        pthread_mutex_unlock(&c->lock);
        return;
    }
    if (ev->type == s_call_event + 1) {
        set_fullscreen_main(ev->user.code ? (void *)1 : NULL);
        return;
    }
    if (ev->type == s_call_event + 3) {
        overlay_changed_main(ev->user.code);
        return;
    }
    if (ev->type == s_call_event + 4) {
        window_size_main(ev->user.code, (int)(intptr_t)ev->user.data1);
        return;
    }
    /* The overlay's own key, a key being learned for a binding, and (while it is up) the
     * pointer and typing. The window's events are in points; the overlay says what it needs. */
    switch (ev->type) {
#if defined(__ANDROID__)
    case SDL_EVENT_FINGER_DOWN: case SDL_EVENT_FINGER_UP: case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
#endif
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL: case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
        int w = 0, h = 0;
        if (s_window)
            SDL_GetWindowSize(s_window, &w, &h);
        if (pc_ui_sdl_event(ev, (float)w, (float)h))
            return;
        break;
    }
    default:
        break;
    }
    switch (ev->type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        /* Closing the window ends the run; the line keeps a deliberate close
         * from reading like a crash. */
        fprintf(stderr, "[TRANS] window closed by the user; exiting (%s)\n",
                ev->type == SDL_EVENT_QUIT ? "quit request: Cmd+Q, the Dock, or Ctrl+C in the terminal"
                                           : "the window's close button");
        fflush(stderr);
        fflush(stdout);
        pc_input_stop();                    /* no pad left vibrating */
        _Exit(0);
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        refresh_drawable();
        if (ev->type == SDL_EVENT_WINDOW_RESIZED && !s_fullscreen && s_window
                && !(SDL_GetWindowFlags(s_window) & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_MINIMIZED))) {
            int w = 0, h = 0;
            SDL_GetWindowSize(s_window, &w, &h);
            if (w > 0 && h > 0) {
                pc_display_set("window_width", w);
                pc_display_set("window_height", h);
            }
        }
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: pc_input_set_focus(1); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:   pc_input_set_focus(0); break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        int down = ev->type == SDL_EVENT_KEY_DOWN;
        /* F11 or Alt+Enter, once per press. */
        if (down && !ev->key.repeat
                && (ev->key.key == SDLK_F11
                    || (ev->key.key == SDLK_RETURN && (ev->key.mod & SDL_KMOD_ALT)))) {
            pc_display_set("fullscreen", !s_fullscreen);
            break;
        }
        if (!ev->key.repeat)
            key_to_input(ev->key.scancode, down);
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        /* VK_LBUTTON 1, VK_RBUTTON 2, VK_MBUTTON 4, VK_XBUTTON1 5, VK_XBUTTON2 6 */
        static const int vk[6] = { 0, 1, 4, 2, 5, 6 };
        if (ev->button.button < 6)
            pc_input_host_key(vk[ev->button.button], ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        pc_input_host_wheel(ev->wheel.y > 0 ? 1 : ev->wheel.y < 0 ? -1 : 0);
        break;
    default:
        break;
    }
}

/* ── A script of input for the window, for tests ───────────────────────── */

/* RECOMP_HOST_SCRIPT="<ms>:key:<name>;<ms>:click:<x>:<y>;<ms>:move:<x>:<y>;<ms>:text:<chars>", times
 * from the moment the window opened and positions in window points, plays the events through the
 * handler as the window's own would arrive. It is how the overlay's keyboard and mouse paths are
 * exercised on a machine with nobody at it (a locked screen, a remote run). A key is a key's SDL
 * name (F1, Escape, A, Space). Off unless the variable is set. */
typedef struct { Uint64 at; int kind; float x, y; char text[32]; int sent; Uint64 release; } ScriptStep;
enum { STEP_KEY, STEP_CLICK, STEP_MOVE, STEP_TEXT };
#define SCRIPT_MAX 64
static ScriptStep s_script[SCRIPT_MAX];
static int        s_script_n;
static Uint64     s_script_start;

static void script_parse(const char *text)
{
    const char *p = text;

    while (p && *p && s_script_n < SCRIPT_MAX) {
        ScriptStep st;
        char kind[16] = "";
        int used = 0;
        unsigned long long ms;

        memset(&st, 0, sizeof st);
        if (sscanf(p, "%llu:%15[a-z]:%n", &ms, kind, &used) >= 2 && used) {
            const char *arg = p + used;
            size_t n = strcspn(arg, ";");
            st.at = ms;
            if (!strcmp(kind, "key") || !strcmp(kind, "text")) {
                st.kind = kind[0] == 'k' ? STEP_KEY : STEP_TEXT;
                snprintf(st.text, sizeof st.text, "%.*s", (int)(n < sizeof st.text - 1 ? n : sizeof st.text - 1), arg);
                s_script[s_script_n++] = st;
            } else if (!strcmp(kind, "click") || !strcmp(kind, "move")) {
                st.kind = kind[0] == 'c' ? STEP_CLICK : STEP_MOVE;
                if (sscanf(arg, "%f:%f", &st.x, &st.y) == 2)
                    s_script[s_script_n++] = st;
            }
        }
        p = strchr(p, ';');
        if (p) p++;
    }
}

/* Play what is due. Called from the first thread's loop. */
static void script_step(void);
static void handle_event(const SDL_Event *ev);

static void script_send(SDL_Event *ev) { handle_event(ev); }

static void script_step(void)
{
    Uint64 now;
    int i;

    if (!s_script_n || !s_window)
        return;
    if (!s_script_start)
        s_script_start = SDL_GetTicks();
    now = SDL_GetTicks() - s_script_start;
    for (i = 0; i < s_script_n; i++) {
        ScriptStep *st = &s_script[i];
        SDL_Event ev;

        if (st->sent == 2 || now < st->at)
            continue;
        SDL_zero(ev);
        if (st->kind == STEP_KEY) {
            SDL_Scancode sc = SDL_GetScancodeFromName(st->text);
            ev.type = st->sent ? SDL_EVENT_KEY_UP : SDL_EVENT_KEY_DOWN;
            ev.key.scancode = sc;
            ev.key.key = SDL_GetKeyFromScancode(sc, 0, false);
            ev.key.down = !st->sent;
            if (!st->sent) { st->release = now + 80; st->sent = 1; script_send(&ev); }
            else if (now >= st->release) { st->sent = 2; script_send(&ev); }
        } else if (st->kind == STEP_TEXT) {
            ev.type = SDL_EVENT_TEXT_INPUT;
            ev.text.text = st->text;
            st->sent = 2;
            script_send(&ev);
        } else if (st->kind == STEP_MOVE || (st->kind == STEP_CLICK && !st->sent)) {
            ev.type = SDL_EVENT_MOUSE_MOTION;
            ev.motion.x = st->x; ev.motion.y = st->y;
            script_send(&ev);
            if (st->kind == STEP_MOVE) { st->sent = 2; }
            else { st->sent = 1; st->release = now + 60; }
        } else if (st->kind == STEP_CLICK && st->sent == 1 && now >= st->release) {
            ev.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            ev.button.button = SDL_BUTTON_LEFT; ev.button.down = true;
            ev.button.x = st->x; ev.button.y = st->y;
            script_send(&ev);
            st->sent = 3; st->release = now + 100;
        } else if (st->kind == STEP_CLICK && st->sent == 3 && now >= st->release) {
            ev.type = SDL_EVENT_MOUSE_BUTTON_UP;
            ev.button.button = SDL_BUTTON_LEFT; ev.button.down = false;
            ev.button.x = st->x; ev.button.y = st->y;
            script_send(&ev);
            st->sent = 2;
        }
    }
}

/* Run the game. With a display: on its own thread, this one serving the
 * window until the game returns. Without: here. */
int host_posix_run(int (*game_main)(void))
{
    pthread_t th;
    pthread_attr_t attr;

#ifdef DEFJAM_VULKAN_LIBRARY
    /* SDL loads the Vulkan loader by name, from places that do not include a
     * package manager's prefix; say where the build found it. */
    if (!getenv("SDL_VULKAN_LIBRARY"))
        SDL_SetHint(SDL_HINT_VULKAN_LIBRARY, DEFJAM_VULKAN_LIBRARY);
#endif
#if defined(__ANDROID__)
    /* SDL reads this only during video initialisation. Java also locks the
     * Activity, but this prevents SDL's sensor listener from restoring portrait. */
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
#endif
    if (getenv("RECOMP_HEADLESS") || !SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        if (!getenv("RECOMP_HEADLESS"))
            fprintf(stderr, "[HOST] no display (%s); running without a window\n", SDL_GetError());
        /* The pads and sound still start SDL, which would turn SIGINT and
         * SIGTERM into a quit event -- and with no window nothing reads
         * events, so the process could not be stopped short of SIGKILL.
         * Leave the signals their default action. */
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        return game_main();
    }
    s_have_video = 1;
    s_call_event = SDL_RegisterEvents(5);
    s_game_main = game_main;

    /* The title's main thread: the stack the first thread would have had. */
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 8u << 20);
    if (pthread_create(&th, &attr, game_thread, NULL) != 0) {
        fprintf(stderr, "[HOST] could not start the game thread\n");
        return 1;
    }
    pthread_attr_destroy(&attr);
    if (getenv("RECOMP_HOST_SCRIPT"))
        script_parse(getenv("RECOMP_HOST_SCRIPT"));
    while (!s_game_done) {
        SDL_Event ev;
        if (SDL_WaitEventTimeout(&ev, s_script_n ? 10 : 100))
            handle_event(&ev);
        script_step();
    }
    pthread_join(th, NULL);
    SDL_Quit();
    return s_game_result;
}

/* ── Debug-register watchpoints: Windows only ──────────────────────────── */

int watchpoint_init(void)
{
    if (getenv("RECOMP_WATCH_WRITE") || getenv("RECOMP_WATCH_EXEC"))
        fprintf(stderr, "[HOST] RECOMP_WATCH_WRITE and RECOMP_WATCH_EXEC use the x86"
                        " debug registers and are not available on this host\n");
    return 0;
}

int watchpoint_handle_trap(void *ctx) { (void)ctx; return 0; }
