/* The PC's pads, keyboard and mouse as Xbox controllers. See input_host.h. */

#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmsystem.h>
#include <xinput.h>

#pragma comment(lib, "winmm.lib")
#else
/* Off Windows there is no XInput: pads are SDL3's or there are none. The
 * threads, events and clocks are the POSIX layer's. */
#include "../platform/win32_compat.h"
#define timeBeginPeriod(ms) ((void)0)
#define timeEndPeriod(ms)   ((void)0)
#endif
#include <stdio.h>
#include <string.h>

#include "input_host.h"

#ifdef XBOXRECOMP_HAVE_SDL3
#include <SDL3/SDL.h>
#endif

#define SLOTS         4
#define MAX_DEVICES   8
#define POLL_MS       2
#define RUMBLE_REFRESH_MS 100
#define RUMBLE_HOLD_MS    300
#define CONNECT_BUZZ_MS   250

typedef struct Device {
    int used;
    int backend;                    /* 1 SDL, 2 XInput */
    int slot;                       /* -1 until assigned */
    char name[64];
#ifdef XBOXRECOMP_HAVE_SDL3
    SDL_Gamepad *gp;
    SDL_JoystickID id;
    SDL_Joystick *vjoy;             /* the virtual pad's joystick, to set its buttons */
#endif
    int is_virtual;
    DWORD xi_index;
} Device;

typedef struct Slot {
    int device;                     /* index into dev[], or -1 */
    WORD low, high;                 /* what the title last asked for */
    WORD applied_low, applied_high; /* what the pad was last told */
    ULONGLONG applied_at;
    ULONGLONG hold_until;           /* the pulse in progress runs at least until then */
    ULONGLONG buzz_until;           /* the buzz for a pad that just connected */
} Slot;

static void (*g_need_players)(int count);

void xbox_HostInputOnPadSlot(void (*need_players)(int count)) { g_need_players = need_players; }

static struct Host {
    int running;
    CRITICAL_SECTION cs;
    HANDLE thread, ready, stop;
    InputHostConfig cfg;
    int kb_slot, kb_merge, players;
    uint8_t key[INPUT_KEY_COUNT];
    uint8_t script_held[INPUT_CONTROL_COUNT];
    ULONGLONG wheel_until[2];
    volatile LONG focused;
    Device dev[MAX_DEVICES];
    Slot slot[SLOTS];
    InputPad out[SLOTS];
    InputPad last[SLOTS];
    InputRaw raw[SLOTS];            /* the pads as read, for a host menu */
    volatile LONG ui_active;
    DWORD packet[SLOTS];
    int found;                      /* pads opened at start */
    int sdl;                        /* SDL is the pad backend */
} H;

void xbox_HostInputDefaults(InputHostConfig *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->keyboard = INPUT_HOST_KEYBOARD_AUTO;
    cfg->use_sdl = 1;
    cfg->rumble_percent = 100;
    cfg->rumble_floor = 60;
    cfg->rumble_min_ms = 200;
    cfg->rumble_on_connect = 1;
    input_padmap_defaults(&cfg->padmap);
}

/* ---- reading a pad into the raw form ------------------------------------- */

static void raw_directions(InputRaw *r)
{
    r->src[INPUT_SRC_LSTICK_UP]    = (int16_t)(r->ly > 0 ? r->ly : 0);
    r->src[INPUT_SRC_LSTICK_DOWN]  = (int16_t)(r->ly < 0 ? -r->ly : 0);
    r->src[INPUT_SRC_LSTICK_LEFT]  = (int16_t)(r->lx < 0 ? -r->lx : 0);
    r->src[INPUT_SRC_LSTICK_RIGHT] = (int16_t)(r->lx > 0 ? r->lx : 0);
    r->src[INPUT_SRC_RSTICK_UP]    = (int16_t)(r->ry > 0 ? r->ry : 0);
    r->src[INPUT_SRC_RSTICK_DOWN]  = (int16_t)(r->ry < 0 ? -r->ry : 0);
    r->src[INPUT_SRC_RSTICK_LEFT]  = (int16_t)(r->rx < 0 ? -r->rx : 0);
    r->src[INPUT_SRC_RSTICK_RIGHT] = (int16_t)(r->rx > 0 ? r->rx : 0);
}

static int16_t clamp_axis(int v)
{
    return (int16_t)(v < -32767 ? -32767 : v > 32767 ? 32767 : v);
}

#if defined(_WIN32)
static int read_xinput(DWORD index, InputRaw *r)
{
    XINPUT_STATE s;
    WORD b;
    if (XInputGetState(index, &s) != ERROR_SUCCESS) return 0;
    b = s.Gamepad.wButtons;
    memset(r, 0, sizeof(*r));
#define BTN(mask, source) if (b & (mask)) r->src[source] = 32767
    BTN(XINPUT_GAMEPAD_A, INPUT_SRC_SOUTH);
    BTN(XINPUT_GAMEPAD_B, INPUT_SRC_EAST);
    BTN(XINPUT_GAMEPAD_X, INPUT_SRC_WEST);
    BTN(XINPUT_GAMEPAD_Y, INPUT_SRC_NORTH);
    BTN(XINPUT_GAMEPAD_BACK, INPUT_SRC_BACK);
    BTN(XINPUT_GAMEPAD_START, INPUT_SRC_START);
    BTN(XINPUT_GAMEPAD_LEFT_THUMB, INPUT_SRC_LEFT_STICK_CLICK);
    BTN(XINPUT_GAMEPAD_RIGHT_THUMB, INPUT_SRC_RIGHT_STICK_CLICK);
    BTN(XINPUT_GAMEPAD_LEFT_SHOULDER, INPUT_SRC_LEFT_SHOULDER);
    BTN(XINPUT_GAMEPAD_RIGHT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER);
    BTN(XINPUT_GAMEPAD_DPAD_UP, INPUT_SRC_DPAD_UP);
    BTN(XINPUT_GAMEPAD_DPAD_DOWN, INPUT_SRC_DPAD_DOWN);
    BTN(XINPUT_GAMEPAD_DPAD_LEFT, INPUT_SRC_DPAD_LEFT);
    BTN(XINPUT_GAMEPAD_DPAD_RIGHT, INPUT_SRC_DPAD_RIGHT);
#undef BTN
    r->src[INPUT_SRC_LEFT_TRIGGER]  = (int16_t)(s.Gamepad.bLeftTrigger * 32767 / 255);
    r->src[INPUT_SRC_RIGHT_TRIGGER] = (int16_t)(s.Gamepad.bRightTrigger * 32767 / 255);
    r->lx = clamp_axis(s.Gamepad.sThumbLX);
    r->ly = clamp_axis(s.Gamepad.sThumbLY);
    r->rx = clamp_axis(s.Gamepad.sThumbRX);
    r->ry = clamp_axis(s.Gamepad.sThumbRY);
    raw_directions(r);
    return 1;
}
#endif

#ifdef XBOXRECOMP_HAVE_SDL3
static void read_sdl(SDL_Gamepad *gp, InputRaw *r)
{
    static const struct { SDL_GamepadButton button; int src; } buttons[] = {
        { SDL_GAMEPAD_BUTTON_SOUTH, INPUT_SRC_SOUTH }, { SDL_GAMEPAD_BUTTON_EAST, INPUT_SRC_EAST },
        { SDL_GAMEPAD_BUTTON_WEST, INPUT_SRC_WEST }, { SDL_GAMEPAD_BUTTON_NORTH, INPUT_SRC_NORTH },
        { SDL_GAMEPAD_BUTTON_BACK, INPUT_SRC_BACK }, { SDL_GAMEPAD_BUTTON_GUIDE, INPUT_SRC_GUIDE },
        { SDL_GAMEPAD_BUTTON_START, INPUT_SRC_START },
        { SDL_GAMEPAD_BUTTON_LEFT_STICK, INPUT_SRC_LEFT_STICK_CLICK },
        { SDL_GAMEPAD_BUTTON_RIGHT_STICK, INPUT_SRC_RIGHT_STICK_CLICK },
        { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, INPUT_SRC_LEFT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER },
        { SDL_GAMEPAD_BUTTON_DPAD_UP, INPUT_SRC_DPAD_UP }, { SDL_GAMEPAD_BUTTON_DPAD_DOWN, INPUT_SRC_DPAD_DOWN },
        { SDL_GAMEPAD_BUTTON_DPAD_LEFT, INPUT_SRC_DPAD_LEFT }, { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, INPUT_SRC_DPAD_RIGHT },
        { SDL_GAMEPAD_BUTTON_MISC1, INPUT_SRC_MISC1 }, { SDL_GAMEPAD_BUTTON_TOUCHPAD, INPUT_SRC_TOUCHPAD },
        { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, INPUT_SRC_PADDLE1 }, { SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, INPUT_SRC_PADDLE2 },
        { SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, INPUT_SRC_PADDLE3 }, { SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, INPUT_SRC_PADDLE4 },
    };
    size_t i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < sizeof(buttons) / sizeof(buttons[0]); i++)
        if (SDL_GetGamepadButton(gp, buttons[i].button)) r->src[buttons[i].src] = 32767;
    {
        int lt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        int rt = SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        r->src[INPUT_SRC_LEFT_TRIGGER]  = (int16_t)(lt < 0 ? 0 : lt);
        r->src[INPUT_SRC_RIGHT_TRIGGER] = (int16_t)(rt < 0 ? 0 : rt);
    }
    /* SDL's Y axes point down; the Xbox's point up. */
    r->lx = clamp_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX));
    r->ly = clamp_axis(-1 - SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY));
    r->rx = clamp_axis(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX));
    r->ry = clamp_axis(-1 - SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY));
    raw_directions(r);
}
#endif

#ifdef XBOXRECOMP_HAVE_SDL3
/* ---- virtual pads (tests) ---------------------------------------------------- */

static SDL_JoystickID g_virtual_ids[SLOTS];
static SDL_Joystick *g_virtual_joys[SLOTS];
static int g_virtual_count;

static bool SDLCALL virtual_rumble(void *userdata, Uint16 low, Uint16 high)
{
    static int logged;
    if (logged++ < 80)
        fprintf(stderr, "[INPUT] virtual pad %d rumble: %u %u\n", (int)(intptr_t)userdata + 1,
                (unsigned)low, (unsigned)high);
    return true;
}

static void attach_virtual_pads(int n)
{
    int i;
    for (i = g_virtual_count; i < n && i < SLOTS; i++) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Virtual test pad";
        desc.userdata = (void *)(intptr_t)i;
        desc.Rumble = virtual_rumble;
        g_virtual_ids[i] = SDL_AttachVirtualJoystick(&desc);
        g_virtual_joys[i] = g_virtual_ids[i] ? SDL_OpenJoystick(g_virtual_ids[i]) : NULL;
        if (g_virtual_ids[i]) g_virtual_count++;
    }
}

static int is_virtual_id(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < SLOTS; i++)
        if (g_virtual_ids[i] == id) return 1;
    return 0;
}

static SDL_Joystick *virtual_joystick(SDL_JoystickID id)
{
    int i;
    for (i = 0; i < SLOTS; i++)
        if (g_virtual_ids[i] == id) return g_virtual_joys[i];
    return NULL;
}
#endif

/* ---- slots and devices ---------------------------------------------------- */

static void log_line(const char *fmt, const char *a, int b)
{
    fprintf(stderr, fmt, a, b);
    fflush(stderr);
}

/* Give a new device the lowest slot the keyboard does not occupy. */
static void assign_device(int d)
{
    int s;
    for (s = 0; s < SLOTS; s++) {
        if (s == H.kb_slot || H.slot[s].device >= 0) continue;
        H.slot[s].device = d;
        H.dev[d].slot = s;
        log_line("[INPUT] %s is player %d\n", H.dev[d].name, s + 1);
        if (g_need_players) g_need_players(s + 1);
        if (H.cfg.rumble_on_connect && !H.dev[d].is_virtual)
            H.slot[s].buzz_until = GetTickCount64() + CONNECT_BUZZ_MS;
        return;
    }
    log_line("[INPUT] %s ignored: all four players have a device\n", H.dev[d].name, 0);
}

static int new_device(int backend, const char *name)
{
    int d;
    for (d = 0; d < MAX_DEVICES; d++) {
        if (H.dev[d].used) continue;
        memset(&H.dev[d], 0, sizeof(H.dev[d]));
        H.dev[d].used = 1;
        H.dev[d].backend = backend;
        H.dev[d].slot = -1;
        snprintf(H.dev[d].name, sizeof(H.dev[d].name), "%s", name ? name : "pad");
        return d;
    }
    return -1;
}

static void free_device(int d)
{
    if (H.dev[d].slot >= 0) {
        log_line("[INPUT] %s removed from player %d\n", H.dev[d].name, H.dev[d].slot + 1);
        H.slot[H.dev[d].slot].device = -1;
    }
#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.dev[d].gp) SDL_CloseGamepad(H.dev[d].gp);
#endif
    memset(&H.dev[d], 0, sizeof(H.dev[d]));
}

static void plan_slots(int pads)
{
    int k = H.cfg.keyboard, n;
    H.kb_slot = -1;
    H.kb_merge = 0;
    if (k == INPUT_HOST_KEYBOARD_MERGE) {
        H.kb_merge = 1;
    } else if (k == INPUT_HOST_KEYBOARD_AUTO) {
        if (pads >= SLOTS) H.kb_merge = 1;
        else H.kb_slot = pads;
    } else if (k >= INPUT_HOST_KEYBOARD_SLOT1 && k <= INPUT_HOST_KEYBOARD_SLOT4) {
        H.kb_slot = k - INPUT_HOST_KEYBOARD_SLOT1;
    }
    n = pads + (H.kb_slot >= 0 ? 1 : 0);
    if (H.kb_slot >= 0 && H.kb_slot + 1 > n) n = H.kb_slot + 1;
    if (n < H.cfg.players) n = H.cfg.players;
    if (n < 1) n = 1;
    if (n > SLOTS) n = SLOTS;
    H.players = n;
}

#ifdef XBOXRECOMP_HAVE_SDL3
/* Steam Input (the Steam Deck's game mode, and any pad Steam owns) shows SDL two
 * pads for one controller: the physical device and the "Steam Virtual Gamepad"
 * Steam builds from it. Both report every press, so each would be a player and
 * the title would see each press twice. The virtual pad is the one Steam wants
 * games to read, so a physical Valve pad is ignored while one exists. */
#define VALVE_VENDOR 0x28de

static int is_steam_virtual(SDL_JoystickID id)
{
    const char *n = SDL_GetGamepadNameForID(id);
    return n && strncmp(n, "Steam Virtual Gamepad", 21) == 0;
}

static int steam_virtual_present(void)
{
    int count = 0, i, found = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (i = 0; ids && i < count && !found; i++) found = is_steam_virtual(ids[i]);
    SDL_free(ids);
    return found;
}

static int is_steam_duplicate(SDL_JoystickID id)
{
    return SDL_GetGamepadVendorForID(id) == VALVE_VENDOR && !is_steam_virtual(id) &&
           steam_virtual_present();
}

static void open_sdl_gamepad(SDL_JoystickID id);

/* After pads come or go: close a physical pad Steam duplicates, open one a
 * departed virtual pad was hiding, and move a Steam pad up to the lowest free
 * player. Other pads are not renumbered: a player's pad does not change under
 * them when someone else's is unplugged. */
static void steam_reconcile(void)
{
    int d, s, count = 0, i;
    SDL_JoystickID *ids;
    EnterCriticalSection(&H.cs);
    for (d = 0; d < MAX_DEVICES; d++)
        if (H.dev[d].used && H.dev[d].backend == 1 && !H.dev[d].is_virtual && is_steam_duplicate(H.dev[d].id)) {
            log_line("[INPUT] %s ignored: Steam's virtual gamepad stands for it\n", H.dev[d].name, 0);
            free_device(d);
        }
    LeaveCriticalSection(&H.cs);
    ids = SDL_GetGamepads(&count);
    for (i = 0; ids && i < count; i++)
        if (!(H.cfg.no_pads && !is_virtual_id(ids[i]))) open_sdl_gamepad(ids[i]);
    SDL_free(ids);
    EnterCriticalSection(&H.cs);
    for (d = 0; d < MAX_DEVICES; d++) {
        if (!H.dev[d].used || H.dev[d].slot < 0) continue;
        /* Only a Steam pad moves up. Any other pad keeps the player it was given. */
        if (H.dev[d].backend != 1 || SDL_GetGamepadVendorForID(H.dev[d].id) != VALVE_VENDOR) continue;
        for (s = 0; s < H.dev[d].slot; s++) {
            if (s == H.kb_slot || H.slot[s].device >= 0) continue;
            H.slot[H.dev[d].slot].device = -1;
            H.slot[s].device = d;
            H.dev[d].slot = s;
            log_line("[INPUT] %s is player %d\n", H.dev[d].name, s + 1);
            break;
        }
    }
    LeaveCriticalSection(&H.cs);
}

static void open_sdl_gamepad(SDL_JoystickID id)
{
    SDL_Gamepad *gp;
    int d, i;
    for (i = 0; i < MAX_DEVICES; i++)
        if (H.dev[i].used && H.dev[i].backend == 1 && H.dev[i].id == id) return;
    if (is_steam_duplicate(id)) return;
    gp = SDL_OpenGamepad(id);
    if (!gp) {
        log_line("[INPUT] could not open pad: %s\n", SDL_GetError(), 0);
        return;
    }
    d = new_device(1, SDL_GetGamepadName(gp));
    if (d < 0) { SDL_CloseGamepad(gp); return; }
    H.dev[d].gp = gp;
    H.dev[d].id = id;
    EnterCriticalSection(&H.cs);
    assign_device(d);
    LeaveCriticalSection(&H.cs);
}
#endif

/* ---- the reader thread ----------------------------------------------------- */

static void apply_rumble(int s, int focused, const InputHostConfig *cfg)
{
    Slot *sl = &H.slot[s];
    Device *dv;
    unsigned low = 0, high = 0, pct = (unsigned)cfg->rumble_percent;
    ULONGLONG now = GetTickCount64();
    DWORD hold_ms;
    if (sl->device < 0) return;
    dv = &H.dev[sl->device];
    if (focused && pct > 0) {
        unsigned floor_v = 65535u * (unsigned)cfg->rumble_floor / 100u;
        low = sl->low;
        high = sl->high;
        /* The title's pulses are brief and often faint; a modern pad's motors
         * need a stronger, longer push than the console's to be felt. */
        if (low && low < floor_v) low = floor_v;
        if (high && high < floor_v) high = floor_v;
        if (now < sl->buzz_until) {
            low = 65535u;
            high = 49152u;
        }
        low = low * pct / 100u;
        high = high * pct / 100u;
        /* Let a pulse finish its minimum length before it is cut short. */
        if (!(low | high) && (sl->applied_low | sl->applied_high) && now < sl->hold_until) {
            low = sl->applied_low;
            high = sl->applied_high;
        }
    }
    if (low == sl->applied_low && high == sl->applied_high &&
        (!(low | high) || now - sl->applied_at < RUMBLE_REFRESH_MS))
        return;
    if ((low | high) && (low != sl->applied_low || high != sl->applied_high))
        sl->hold_until = now + (ULONGLONG)(cfg->rumble_min_ms > 0 ? cfg->rumble_min_ms : 0);
    sl->applied_low = (WORD)low;
    sl->applied_high = (WORD)high;
    sl->applied_at = now;
    hold_ms = RUMBLE_HOLD_MS;
    if ((DWORD)cfg->rumble_min_ms + 100 > hold_ms) hold_ms = (DWORD)cfg->rumble_min_ms + 100;
#ifdef XBOXRECOMP_HAVE_SDL3
    if (dv->backend == 1 && dv->gp) {
        static int logged;
        bool ok = SDL_RumbleGamepad(dv->gp, (Uint16)low, (Uint16)high, (low | high) ? hold_ms : 0);
        if ((low | high) && logged < 12) {
            logged++;
            fprintf(stderr, "[INPUT] rumble %u %u on player %d (%s): %s%s\n", low, high,
                    s + 1, dv->name, ok ? "sent" : "FAILED ", ok ? "" : SDL_GetError());
            fflush(stderr);
        }
        return;
    }
#endif
#if defined(_WIN32)
    if (dv->backend == 2) {
        XINPUT_VIBRATION v;
        v.wLeftMotorSpeed = (WORD)low;
        v.wRightMotorSpeed = (WORD)high;
        XInputSetState(dv->xi_index, &v);
    }
#endif
}

static void to_state(const InputPad *p, XBOX_INPUT_STATE *out, DWORD packet)
{
    memset(out, 0, sizeof(*out));
    out->dwPacketNumber = packet;
    out->Gamepad.wButtons = (WORD)(p->buttons & 0xFF);
    memcpy(out->Gamepad.bAnalogButtons, p->analog, 8);
    out->Gamepad.sThumbLX = p->lx;
    out->Gamepad.sThumbLY = p->ly;
    out->Gamepad.sThumbRX = p->rx;
    out->Gamepad.sThumbRY = p->ry;
}

static void poll_once(void)
{
    InputPad next[SLOTS];
    InputRaw raws[SLOTS];
    uint8_t keys[INPUT_KEY_COUNT];
    InputHostConfig cfg;
    ULONGLONG now = GetTickCount64();
    int focused = InterlockedCompareExchange(&H.focused, 0, 0);
    int ui = InterlockedCompareExchange(&H.ui_active, 0, 0);
    int s, live;

    EnterCriticalSection(&H.cs);
    cfg = H.cfg;
    memcpy(keys, H.key, sizeof(keys));
    for (s = 0; s < INPUT_CONTROL_COUNT; s++)
        if (H.script_held[s] && cfg.keys.count[s]) keys[cfg.keys.key[s][0]] = 1;
    LeaveCriticalSection(&H.cs);
    live = focused || cfg.ignore_focus;
    keys[INPUT_KEY_WHEEL_UP] = now < H.wheel_until[0];
    keys[INPUT_KEY_WHEEL_DOWN] = now < H.wheel_until[1];

    memset(next, 0, sizeof(next));
    memset(raws, 0, sizeof(raws));
    for (s = 0; s < SLOTS; s++) {
        InputRaw raw;
        Device *dv;
        int have = 0;
        if (H.slot[s].device < 0) continue;
        dv = &H.dev[H.slot[s].device];
#ifdef XBOXRECOMP_HAVE_SDL3
        if (dv->backend == 1 && dv->gp) { read_sdl(dv->gp, &raw); have = 1; }
#endif
#if defined(_WIN32)
        if (dv->backend == 2) have = read_xinput(dv->xi_index, &raw);
#endif
        if (have && live) {
            raws[s] = raw;
            if (!ui) input_pad_map(&cfg.padmap, &raw, &next[s]);
        }
    }
    if (live && !ui) {
        InputPad kp;
        if (H.kb_slot >= 0 || H.kb_merge) {
            input_keys_resolve(&cfg.keys, keys, &kp);
            if (H.kb_slot >= 0) next[H.kb_slot] = kp;
            else input_pad_merge(&next[0], &kp);
        }
    }

    EnterCriticalSection(&H.cs);
    for (s = 0; s < SLOTS; s++) {
        if (memcmp(&next[s], &H.last[s], sizeof(InputPad)) != 0) {
            H.last[s] = next[s];
            H.packet[s]++;
        }
        H.out[s] = next[s];
        H.raw[s] = raws[s];
    }
    LeaveCriticalSection(&H.cs);

    for (s = 0; s < SLOTS; s++)
        apply_rumble(s, live && (!ui || GetTickCount64() < H.slot[s].buzz_until), &cfg);
}

#if defined(XBOXRECOMP_HAVE_SDL3) && !defined(_WIN32)
/* Off Windows the program's window is SDL's too, and its own thread takes
 * events off the same queue. So pads arriving and leaving are noted by a
 * watch, which sees every event whoever ends up reading it, and this thread
 * only removes the joystick and gamepad events from the queue. */
#define PAD_EVENTS 32
static struct { SDL_JoystickID id; int added; } g_pad_event[PAD_EVENTS];
static int g_pad_events;
static SDL_SpinLock g_pad_event_lock;

static bool SDLCALL pad_watch(void *user, SDL_Event *e)
{
    (void)user;
    if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED) {
        SDL_LockSpinlock(&g_pad_event_lock);
        if (g_pad_events < PAD_EVENTS) {
            g_pad_event[g_pad_events].id = e->gdevice.which;
            g_pad_event[g_pad_events].added = e->type == SDL_EVENT_GAMEPAD_ADDED;
            g_pad_events++;
        }
        SDL_UnlockSpinlock(&g_pad_event_lock);
    }
    return true;
}
#endif

static void hotplug_once(void)
{
    int d;
#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.sdl) {
        int changed = 0;
#if defined(_WIN32)
        SDL_Event e;
        SDL_UpdateGamepads();
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_GAMEPAD_ADDED || e.type == SDL_EVENT_GAMEPAD_REMOVED) changed = 1;
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                if (!(H.cfg.no_pads && !is_virtual_id(e.gdevice.which)))
                    open_sdl_gamepad(e.gdevice.which);
            } else if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
                EnterCriticalSection(&H.cs);
                for (d = 0; d < MAX_DEVICES; d++)
                    if (H.dev[d].used && H.dev[d].backend == 1 && H.dev[d].id == e.gdevice.which)
                        free_device(d);
                LeaveCriticalSection(&H.cs);
            }
        }
#else
        SDL_Event drop[16];
        int n, k;
        SDL_UpdateGamepads();
        while (SDL_PeepEvents(drop, 16, SDL_GETEVENT, SDL_EVENT_JOYSTICK_AXIS_MOTION,
                              SDL_EVENT_GAMEPAD_STEAM_HANDLE_UPDATED) > 0)
            ;
        for (;;) {
            SDL_JoystickID id = 0;
            int added = 0;
            SDL_LockSpinlock(&g_pad_event_lock);
            n = g_pad_events;
            if (n) {
                id = g_pad_event[0].id;
                added = g_pad_event[0].added;
                for (k = 1; k < n; k++) g_pad_event[k - 1] = g_pad_event[k];
                g_pad_events = n - 1;
            }
            SDL_UnlockSpinlock(&g_pad_event_lock);
            if (!n) break;
            changed = 1;
            if (added) {
                if (!(H.cfg.no_pads && !is_virtual_id(id)))
                    open_sdl_gamepad(id);
            } else {
                EnterCriticalSection(&H.cs);
                for (d = 0; d < MAX_DEVICES; d++)
                    if (H.dev[d].used && H.dev[d].backend == 1 && H.dev[d].id == id)
                        free_device(d);
                LeaveCriticalSection(&H.cs);
            }
        }
#endif
        if (changed) steam_reconcile();
        return;
    }
#endif
#if defined(_WIN32)
    {
    static ULONGLONG xi_scan;
    ULONGLONG now = GetTickCount64();
    if (H.cfg.no_pads || now - xi_scan < 1000) return;
    xi_scan = now;
    for (d = 0; d < XUSER_MAX_COUNT; d++) {
        InputRaw raw;
        int i, known = -1;
        for (i = 0; i < MAX_DEVICES; i++)
            if (H.dev[i].used && H.dev[i].backend == 2 && H.dev[i].xi_index == (DWORD)d) known = i;
        if (read_xinput((DWORD)d, &raw)) {
            if (known < 0) {
                char name[32];
                snprintf(name, sizeof(name), "XInput pad %d", d + 1);
                known = new_device(2, name);
                if (known >= 0) {
                    H.dev[known].xi_index = (DWORD)d;
                    EnterCriticalSection(&H.cs);
                    assign_device(known);
                    LeaveCriticalSection(&H.cs);
                }
            }
        } else if (known >= 0) {
            EnterCriticalSection(&H.cs);
            free_device(known);
            LeaveCriticalSection(&H.cs);
        }
    }
    }
#else
    (void)d;
#endif
}

static DWORD WINAPI reader_thread(LPVOID unused)
{
    int d, i;
    (void)unused;
    timeBeginPeriod(1);

#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.cfg.use_sdl && (!H.cfg.no_pads || H.cfg.virtual_pads)) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (SDL_Init(SDL_INIT_GAMEPAD)) {
            H.sdl = 1;
#if !defined(_WIN32)
            SDL_AddEventWatch(pad_watch, NULL);
#endif
            attach_virtual_pads(H.cfg.virtual_pads);
        } else {
            log_line("[INPUT] SDL gamepads unavailable (%s); using XInput\n", SDL_GetError(), 0);
        }
    }
    if (H.sdl) {
        /* Pads can take a moment to appear; wait until the count holds still. */
        int stable = 0, last = -1, waited;
        SDL_JoystickID *ids = NULL;
        int count = 0;
        for (waited = 0; waited < 400 && stable < 100; waited += 25) {
            SDL_UpdateGamepads();
            ids = SDL_GetGamepads(&count);
            SDL_free(ids);
            stable = count == last ? stable + 25 : 0;
            last = count;
            if (count > 0 && stable >= 100) break;
            SDL_Delay(25);
        }
        ids = SDL_GetGamepads(&count);
        for (i = 0; ids && i < count && i < MAX_DEVICES; i++) {
            SDL_Gamepad *gp;
            if (H.cfg.no_pads && !is_virtual_id(ids[i])) continue;
            if (is_steam_duplicate(ids[i])) {
                fprintf(stderr, "[INPUT] %s ignored: Steam's virtual gamepad stands for it\n",
                        SDL_GetGamepadNameForID(ids[i]));
                continue;
            }
            gp = SDL_OpenGamepad(ids[i]);
            if (!gp) continue;
            d = new_device(1, SDL_GetGamepadName(gp));
            if (d < 0) { SDL_CloseGamepad(gp); continue; }
            H.dev[d].gp = gp;
            H.dev[d].id = ids[i];
            H.dev[d].is_virtual = is_virtual_id(ids[i]);
            H.dev[d].vjoy = virtual_joystick(ids[i]);
            H.found++;
        }
        SDL_free(ids);
    }
#endif
#if defined(_WIN32)
    if (!H.sdl && !H.cfg.no_pads) {
        for (i = 0; i < XUSER_MAX_COUNT; i++) {
            InputRaw raw;
            if (read_xinput((DWORD)i, &raw)) {
                char name[32];
                snprintf(name, sizeof(name), "XInput pad %d", i + 1);
                d = new_device(2, name);
                if (d >= 0) { H.dev[d].xi_index = (DWORD)i; H.found++; }
            }
        }
    }
#endif

    EnterCriticalSection(&H.cs);
    plan_slots(H.found > SLOTS ? SLOTS : H.found);
    for (d = 0; d < MAX_DEVICES; d++)
        if (H.dev[d].used) assign_device(d);
    LeaveCriticalSection(&H.cs);
    fprintf(stderr, "[INPUT] %d pad(s) found (%s); keyboard %s; %d player(s)\n", H.found,
            H.sdl ? "SDL" : "XInput",
            H.kb_slot >= 0 ? "its own player" : H.kb_merge ? "merged into player 1" : "off",
            H.players);
    if (H.kb_slot >= 0) fprintf(stderr, "[INPUT] keyboard and mouse are player %d\n", H.kb_slot + 1);
    fflush(stderr);
    SetEvent(H.ready);

    {
        ULONGLONG started = GetTickCount64();
        int late_done = 0;
        while (WaitForSingleObject(H.stop, POLL_MS) == WAIT_TIMEOUT) {
#ifdef XBOXRECOMP_HAVE_SDL3
            /* Test hook: one more virtual pad arrives while the game runs. */
            if (H.sdl && H.cfg.virtual_late_ms && !late_done &&
                GetTickCount64() - started >= (ULONGLONG)H.cfg.virtual_late_ms) {
                late_done = 1;
                attach_virtual_pads(g_virtual_count + 1);
                fprintf(stderr, "[INPUT] a virtual pad was plugged in\n");
                fflush(stderr);
            }
#else
            (void)started; (void)late_done;
#endif
#ifdef XBOXRECOMP_HAVE_SDL3
            if (H.sdl && H.cfg.virtual_chord_ms && g_virtual_joys[0]) {
                ULONGLONG t = GetTickCount64() - started;
                ULONGLONG c = (ULONGLONG)H.cfg.virtual_chord_ms;
                bool clicks = t >= c && t < c + 1800;
                bool east = t >= c + 3500 && t < c + 3800;
                SDL_SetJoystickVirtualButton(g_virtual_joys[0], SDL_GAMEPAD_BUTTON_LEFT_STICK, clicks);
                SDL_SetJoystickVirtualButton(g_virtual_joys[0], SDL_GAMEPAD_BUTTON_RIGHT_STICK, clicks);
                SDL_SetJoystickVirtualButton(g_virtual_joys[0], SDL_GAMEPAD_BUTTON_EAST, east);
            }
#endif
            hotplug_once();
            poll_once();
        }
    }

    /* Stop the motors before the devices go. */
    EnterCriticalSection(&H.cs);
    for (i = 0; i < SLOTS; i++) { H.slot[i].low = H.slot[i].high = 0; }
    LeaveCriticalSection(&H.cs);
    for (i = 0; i < SLOTS; i++) apply_rumble(i, 0, &H.cfg);
    EnterCriticalSection(&H.cs);
    for (d = 0; d < MAX_DEVICES; d++)
        if (H.dev[d].used) free_device(d);
    LeaveCriticalSection(&H.cs);
#ifdef XBOXRECOMP_HAVE_SDL3
    if (H.sdl) {
        for (i = 0; i < SLOTS; i++) {
            if (g_virtual_joys[i]) SDL_CloseJoystick(g_virtual_joys[i]);
            if (g_virtual_ids[i]) SDL_DetachVirtualJoystick(g_virtual_ids[i]);
            g_virtual_joys[i] = NULL;
            g_virtual_ids[i] = 0;
        }
        g_virtual_count = 0;
#if !defined(_WIN32)
        SDL_RemoveEventWatch(pad_watch, NULL);
#endif
        SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    }
#endif
    timeEndPeriod(1);
    return 0;
}

/* ---- the public interface ---------------------------------------------------- */

int xbox_HostInputStart(const InputHostConfig *cfg)
{
    static int cs_ready;
    int i;
    if (H.running) return H.found;
    if (!cs_ready) { InitializeCriticalSection(&H.cs); cs_ready = 1; }
    EnterCriticalSection(&H.cs);
    memset(H.dev, 0, sizeof(H.dev));
    memset(H.slot, 0, sizeof(H.slot));
    memset(H.out, 0, sizeof(H.out));
    memset(H.last, 0, sizeof(H.last));
    memset(H.packet, 0, sizeof(H.packet));
    memset(H.key, 0, sizeof(H.key));
    memset(H.script_held, 0, sizeof(H.script_held));
    H.wheel_until[0] = H.wheel_until[1] = 0;
    for (i = 0; i < SLOTS; i++) H.slot[i].device = -1;
    H.found = 0;
    H.sdl = 0;
    H.cfg = *cfg;
    LeaveCriticalSection(&H.cs);
    InterlockedExchange(&H.focused, 1);
    H.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    H.stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    H.thread = H.ready && H.stop ? CreateThread(NULL, 0, reader_thread, NULL, 0, NULL) : NULL;
    if (!H.thread) return 0;
    H.running = 1;
    WaitForSingleObject(H.ready, 10000);
    return H.found;
}

void xbox_HostInputStop(void)
{
    if (!H.running) return;
    H.running = 0;
    SetEvent(H.stop);
    WaitForSingleObject(H.thread, 5000);
    CloseHandle(H.thread);
    CloseHandle(H.ready);
    CloseHandle(H.stop);
    H.thread = H.ready = H.stop = NULL;
}

int xbox_HostInputActive(void) { return H.running; }

void xbox_HostInputConfigure(const InputHostConfig *cfg)
{
    if (!H.running) return;
    EnterCriticalSection(&H.cs);
    /* The layout fixed at start stays. */
    {
        int kb = H.cfg.keyboard, players = H.cfg.players;
        H.cfg = *cfg;
        H.cfg.keyboard = kb;
        H.cfg.players = players;
    }
    LeaveCriticalSection(&H.cs);
}

int xbox_HostInputSlotIsVirtual(int slot)
{
    int v = 0;
    if (!H.running || slot < 0 || slot >= SLOTS) return 0;
    EnterCriticalSection(&H.cs);
    if (H.slot[slot].device >= 0) v = H.dev[H.slot[slot].device].is_virtual;
    LeaveCriticalSection(&H.cs);
    return v;
}

void xbox_HostInputScriptVirtual(int slot, const uint8_t *held)
{
#ifdef XBOXRECOMP_HAVE_SDL3
    /* Control order: d-pad, start, back, thumbs, A B X Y Black White, triggers,
     * then the sticks' four directions each. */
    static const int buttons[16] = {
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN,
        SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST,
        SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        -1, -1 };
    SDL_Joystick *joy = NULL;
    int c, v;
    if (!H.running || slot < 0 || slot >= SLOTS || !held) return;
    EnterCriticalSection(&H.cs);
    if (H.slot[slot].device >= 0) joy = H.dev[H.slot[slot].device].vjoy;
    LeaveCriticalSection(&H.cs);
    if (!joy) return;
    for (c = 0; c < 16; c++)
        if (buttons[c] >= 0) SDL_SetJoystickVirtualButton(joy, buttons[c], held[c] != 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, held[INPUT_LEFT_TRIGGER] ? 32767 : 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, held[INPUT_RIGHT_TRIGGER] ? 32767 : 0);
    /* SDL's Y axes point down. */
    v = (held[INPUT_LSTICK_RIGHT] ? 32767 : 0) - (held[INPUT_LSTICK_LEFT] ? 32767 : 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTX, (Sint16)v);
    v = (held[INPUT_LSTICK_DOWN] ? 32767 : 0) - (held[INPUT_LSTICK_UP] ? 32767 : 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_LEFTY, (Sint16)v);
    v = (held[INPUT_RSTICK_RIGHT] ? 32767 : 0) - (held[INPUT_RSTICK_LEFT] ? 32767 : 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTX, (Sint16)v);
    v = (held[INPUT_RSTICK_DOWN] ? 32767 : 0) - (held[INPUT_RSTICK_UP] ? 32767 : 0);
    SDL_SetJoystickVirtualAxis(joy, SDL_GAMEPAD_AXIS_RIGHTY, (Sint16)v);
#else
    (void)slot; (void)held;
#endif
}

void xbox_HostInputSetUiActive(int active)
{
    InterlockedExchange(&H.ui_active, active ? 1 : 0);
}

void xbox_HostInputRumbleTest(int slot)
{
    if (!H.running || slot < 0 || slot >= SLOTS) return;
    H.slot[slot].buzz_until = GetTickCount64() + 400;
}

int xbox_HostInputUiActive(void) { return InterlockedCompareExchange(&H.ui_active, 0, 0); }

int xbox_HostInputRawPad(int slot, InputRaw *raw)
{
    int have = 0;
    if (raw) memset(raw, 0, sizeof(*raw));
    if (!H.running || slot < 0 || slot >= SLOTS || !raw) return 0;
    EnterCriticalSection(&H.cs);
    have = H.slot[slot].device >= 0;
    if (have) *raw = H.raw[slot];
    LeaveCriticalSection(&H.cs);
    return have;
}

void xbox_HostInputScriptControls(const uint8_t *held)
{
    if (!H.running || !held) return;
    EnterCriticalSection(&H.cs);
    memcpy(H.script_held, held, sizeof(H.script_held));
    LeaveCriticalSection(&H.cs);
}

int xbox_HostInputPlayerCount(void) { return H.players < 1 ? 1 : H.players; }
int xbox_HostInputKeyboardSlot(void) { return H.kb_slot; }

void xbox_HostInputKey(int code, int down)
{
    if (code >= 0 && code < INPUT_KEY_COUNT) H.key[code] = down ? 1 : 0;
}

void xbox_HostInputWheel(int notches)
{
    if (notches > 0) H.wheel_until[0] = GetTickCount64() + 120;
    if (notches < 0) H.wheel_until[1] = GetTickCount64() + 120;
}

void xbox_HostInputFocus(int focused)
{
    InterlockedExchange(&H.focused, focused ? 1 : 0);
    if (!focused) memset(H.key, 0, sizeof(H.key));
}

DWORD xbox_HostInputGetState(int slot, XBOX_INPUT_STATE *state)
{
    int present;
    if (!H.running || slot < 0 || slot >= SLOTS || !state) return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&H.cs);
    present = H.slot[slot].device >= 0 || slot == H.kb_slot || (H.kb_merge && slot == 0);
    if (present) to_state(&H.out[slot], state, H.packet[slot]);
    LeaveCriticalSection(&H.cs);
    return present ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED;
}

DWORD xbox_HostInputRumble(int slot, WORD low, WORD high)
{
    if (!H.running || slot < 0 || slot >= SLOTS) return ERROR_DEVICE_NOT_CONNECTED;
    EnterCriticalSection(&H.cs);
    H.slot[slot].low = low;
    H.slot[slot].high = high;
    LeaveCriticalSection(&H.cs);
    return ERROR_SUCCESS;
}

int xbox_HostInputSlotInfo(int slot, char *name, int size)
{
    int what = 0;
    if (!H.running || slot < 0 || slot >= SLOTS) return 0;
    EnterCriticalSection(&H.cs);
    if (H.slot[slot].device >= 0) what |= 1;
    if (slot == H.kb_slot || (H.kb_merge && slot == 0)) what |= 2;
    if (name && size > 0) {
        if (what == 1) snprintf(name, (size_t)size, "%s", H.dev[H.slot[slot].device].name);
        else if (what == 2) snprintf(name, (size_t)size, "Keyboard and mouse");
        else if (what == 3) snprintf(name, (size_t)size, "%s + keyboard", H.dev[H.slot[slot].device].name);
        else name[0] = 0;
    }
    LeaveCriticalSection(&H.cs);
    return what;
}
