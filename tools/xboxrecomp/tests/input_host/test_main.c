#define _CRT_SECURE_NO_WARNINGS
/* input_host against SDL3's virtual gamepads: slot assignment, mapping,
 * deadzone, the keyboard as its own player or merged, focus, wheel notches,
 * hot-plug, and rumble reaching the pad. */

#include "input_host.h"

#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } } while (0)

static volatile unsigned g_rumble_low, g_rumble_high;
static volatile int g_rumble_calls;

static bool SDLCALL on_rumble(void *userdata, Uint16 low, Uint16 high)
{
    (void)userdata;
    g_rumble_low = low;
    g_rumble_high = high;
    g_rumble_calls++;
    return true;
}

static volatile int g_need_players;
static void need_players(int count) { if (count > g_need_players) g_need_players = count; }

typedef struct VPad { SDL_JoystickID id; SDL_Joystick *joy; } VPad;

static VPad attach(const char *name)
{
    VPad p;
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = name;
    desc.Rumble = on_rumble;
    p.id = SDL_AttachVirtualJoystick(&desc);
    p.joy = p.id ? SDL_OpenJoystick(p.id) : NULL;
    return p;
}

static void settle(void) { Sleep(80); }

static XBOX_INPUT_STATE get(int slot, DWORD *rc)
{
    XBOX_INPUT_STATE s;
    memset(&s, 0, sizeof(s));
    *rc = xbox_HostInputGetState(slot, &s);
    return s;
}

static void keys_for_test(InputHostConfig *cfg)
{
    input_bindings_parse(&cfg->keys, INPUT_A, "J");
    input_bindings_parse(&cfg->keys, INPUT_LSTICK_RIGHT, "D");
    input_bindings_parse(&cfg->keys, INPUT_DPAD_UP, "WheelUp");
    input_bindings_parse(&cfg->keys, INPUT_START, "Enter");
}

int main(void)
{
    InputHostConfig cfg;
    XBOX_INPUT_STATE s;
    DWORD rc, packet;
    VPad pad1, pad2;
    char name[64];

    xbox_HostInputOnPadSlot(need_players);
    CHECK(SDL_Init(SDL_INIT_GAMEPAD));
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    /* ---- one pad and a keyboard: the keyboard is player 2 ---- */
    pad1 = attach("Test pad 1");
    CHECK(pad1.id != 0 && pad1.joy != NULL);
    CHECK(SDL_IsGamepad(pad1.id));

    xbox_HostInputDefaults(&cfg);
    keys_for_test(&cfg);
    /* The exact-value checks below want the title's rumble passed through as it is. */
    cfg.rumble_floor = 0;
    cfg.rumble_min_ms = 0;
    cfg.rumble_on_connect = 0;
    CHECK(xbox_HostInputStart(&cfg) == 1);
    CHECK(xbox_HostInputActive());
    CHECK(xbox_HostInputPlayerCount() == 2);
    CHECK(xbox_HostInputKeyboardSlot() == 1);
    CHECK(g_need_players == 1);                       /* the pad in slot 0 needs one controller */
    CHECK(xbox_HostInputSlotInfo(0, name, sizeof(name)) == 1 && strstr(name, "Test pad 1"));
    CHECK(xbox_HostInputSlotInfo(1, name, sizeof(name)) == 2);
    CHECK(xbox_HostInputSlotInfo(2, NULL, 0) == 0);

    s = get(0, &rc);
    CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    packet = s.dwPacketNumber;

    /* A pad press arrives as the A button, and moves the packet number. */
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_LEFTY, -32768);   /* SDL: up is negative */
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_RIGHTX, 2000);    /* inside the deadzone */
    settle();
    s = get(0, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(s.Gamepad.sThumbLX == 0 && s.Gamepad.sThumbLY == 32767);   /* full up, Y flipped to the Xbox's */
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_RTRIGGER] == 255);
    CHECK(s.Gamepad.sThumbRX == 0);
    CHECK(s.dwPacketNumber != packet);
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_LEFTY, 0);
    SDL_SetJoystickVirtualAxis(pad1.joy, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    settle();

    /* The keyboard drives its own slot and leaves the pad's alone. */
    xbox_HostInputKey('J', 1);
    xbox_HostInputKey('D', 1);
    xbox_HostInputKey(0x0D, 1);
    settle();
    s = get(1, &rc);
    CHECK(rc == ERROR_SUCCESS);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    CHECK(s.Gamepad.sThumbLX == 32767);
    CHECK(s.Gamepad.wButtons & XBOX_GAMEPAD_START);
    s = get(0, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0 && !(s.Gamepad.wButtons & XBOX_GAMEPAD_START));

    /* A host menu open: the title polls everything at rest, the menu still reads the pads, and a
     * rumble test still gets through while the title's own rumble does not. */
    {
        InputRaw raw;
        SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
        xbox_HostInputSetUiActive(1);
        CHECK(xbox_HostInputUiActive() == 1);
        settle();
        s = get(0, &rc);
        CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
        s = get(1, &rc);
        CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0 && s.Gamepad.sThumbLX == 0);
        CHECK(xbox_HostInputRawPad(0, &raw) == 1);
        CHECK(raw.src[INPUT_SRC_SOUTH] > 16384);
        CHECK(xbox_HostInputRawPad(1, &raw) == 0);               /* the keyboard's slot has no pad */
        g_rumble_low = g_rumble_high = 0;
        xbox_HostInputRumble(0, 0xFFFF, 0xFFFF);
        settle();
        CHECK(g_rumble_low == 0);                                /* the title's rumble is held back */
        xbox_HostInputRumble(0, 0, 0);
        xbox_HostInputRumbleTest(0);
        settle();
        CHECK(g_rumble_low == 0xFFFF);                           /* the menu's test buzz is not */
        Sleep(500);
        CHECK(g_rumble_low == 0 && g_rumble_high == 0);
        xbox_HostInputSetUiActive(0);
        settle();
        s = get(0, &rc);
        CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);   /* back to the game, still held */
        SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
        settle();
    }

    /* Losing the focus reads as everything released, and keys do not stick. */
    xbox_HostInputFocus(0);
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    settle();
    s = get(1, &rc);
    CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0 && s.Gamepad.sThumbLX == 0);
    s = get(0, &rc);
    CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    xbox_HostInputFocus(1);
    settle();
    s = get(1, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);         /* the key was released with the focus */
    s = get(0, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);       /* the pad is still held */
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, false);

    /* A wheel notch is a short press. */
    xbox_HostInputWheel(1);
    Sleep(30);
    s = get(1, &rc);
    CHECK(s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_UP);
    Sleep(250);
    s = get(1, &rc);
    CHECK(!(s.Gamepad.wButtons & XBOX_GAMEPAD_DPAD_UP));

    /* Rumble: scaled by the setting, forwarded to the pad in the slot, stopped
     * by a zero and by losing the focus. */
    g_rumble_calls = 0;
    CHECK(xbox_HostInputRumble(0, 0xFFFF, 0x8000) == ERROR_SUCCESS);
    settle();
    CHECK(g_rumble_calls > 0 && g_rumble_low == 0xFFFF && g_rumble_high == 0x8000);
    Sleep(300);                                          /* kept alive past the pad's own timeout */
    CHECK(g_rumble_low == 0xFFFF);
    xbox_HostInputFocus(0);
    settle();
    CHECK(g_rumble_low == 0 && g_rumble_high == 0);
    xbox_HostInputFocus(1);
    settle();
    CHECK(g_rumble_low == 0xFFFF);
    xbox_HostInputRumble(0, 0, 0);
    settle();
    CHECK(g_rumble_low == 0 && g_rumble_high == 0);
    cfg.rumble_percent = 50;
    xbox_HostInputConfigure(&cfg);
    xbox_HostInputRumble(0, 0x8000, 0x4000);
    settle();
    CHECK(g_rumble_low == 0x4000 && g_rumble_high == 0x2000);
    cfg.rumble_percent = 0;
    xbox_HostInputConfigure(&cfg);
    settle();
    CHECK(g_rumble_low == 0 && g_rumble_high == 0);
    xbox_HostInputRumble(0, 0, 0);
    cfg.rumble_percent = 100;
    xbox_HostInputConfigure(&cfg);

    /* A faint, brief pulse is made felt: raised to the floor and held for the
     * minimum length, then released. */
    cfg.rumble_floor = 60;
    cfg.rumble_min_ms = 250;
    xbox_HostInputConfigure(&cfg);
    xbox_HostInputRumble(0, 0x4000, 0);
    settle();
    CHECK(g_rumble_low == 0x9999 && g_rumble_high == 0);          /* 60% of full */
    xbox_HostInputRumble(0, 0, 0);                                /* the title stops it at once */
    settle();
    CHECK(g_rumble_low == 0x9999);                                /* still running: under 250 ms */
    Sleep(300);
    CHECK(g_rumble_low == 0 && g_rumble_high == 0);
    xbox_HostInputRumble(0, 0xFFFF, 0);                           /* strong pulses pass unchanged */
    settle();
    CHECK(g_rumble_low == 0xFFFF);
    xbox_HostInputRumble(0, 0, 0);
    Sleep(300);
    cfg.rumble_floor = 0;
    cfg.rumble_min_ms = 0;
    xbox_HostInputConfigure(&cfg);

    /* Remapping applies live: swap A and B. */
    cfg.padmap.source[INPUT_A] = INPUT_SRC_EAST;
    cfg.padmap.source[INPUT_B] = INPUT_SRC_SOUTH;
    xbox_HostInputConfigure(&cfg);
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, true);
    settle();
    s = get(0, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255 && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_SOUTH, false);
    input_padmap_defaults(&cfg.padmap);
    xbox_HostInputConfigure(&cfg);

    /* Hot-plug: a second pad takes slot 2 (index), skipping the keyboard's 1. */
    pad2 = attach("Test pad 2");
    settle();
    CHECK(xbox_HostInputSlotInfo(2, name, sizeof(name)) == 1);
    CHECK(g_need_players == 3);                       /* the host is asked to plug a third controller in */
    SDL_SetJoystickVirtualButton(pad2.joy, SDL_GAMEPAD_BUTTON_EAST, true);
    settle();
    s = get(2, &rc);
    CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);

    /* Unplugging pad 1 empties its slot; the keyboard stays. */
    SDL_CloseJoystick(pad1.joy);
    SDL_DetachVirtualJoystick(pad1.id);
    settle();
    s = get(0, &rc);
    CHECK(rc == ERROR_DEVICE_NOT_CONNECTED);
    s = get(1, &rc);
    CHECK(rc == ERROR_SUCCESS);
    SDL_CloseJoystick(pad2.joy);
    SDL_DetachVirtualJoystick(pad2.id);
    xbox_HostInputStop();
    CHECK(!xbox_HostInputActive());

    /* ---- no pad: the keyboard is player 1 ---- */
    xbox_HostInputDefaults(&cfg);
    keys_for_test(&cfg);
    CHECK(xbox_HostInputStart(&cfg) == 0);
    CHECK(xbox_HostInputPlayerCount() == 1 && xbox_HostInputKeyboardSlot() == 0);
    xbox_HostInputKey('J', 1);
    settle();
    s = get(0, &rc);
    CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    xbox_HostInputStop();

    /* ---- merged: the keyboard adds to player 1's pad ---- */
    pad1 = attach("Test pad 1");
    xbox_HostInputDefaults(&cfg);
    cfg.keyboard = INPUT_HOST_KEYBOARD_MERGE;
    keys_for_test(&cfg);
    CHECK(xbox_HostInputStart(&cfg) == 1);
    CHECK(xbox_HostInputPlayerCount() == 1 && xbox_HostInputKeyboardSlot() == -1);
    xbox_HostInputKey('J', 1);
    SDL_SetJoystickVirtualButton(pad1.joy, SDL_GAMEPAD_BUTTON_EAST, true);
    settle();
    s = get(0, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255 && s.Gamepad.bAnalogButtons[XBOX_BUTTON_B] == 255);
    xbox_HostInputStop();

    /* ---- keyboard off, players forced up ---- */
    xbox_HostInputDefaults(&cfg);
    cfg.keyboard = INPUT_HOST_KEYBOARD_OFF;
    cfg.players = 3;
    CHECK(xbox_HostInputStart(&cfg) == 1);
    CHECK(xbox_HostInputPlayerCount() == 3 && xbox_HostInputKeyboardSlot() == -1);
    xbox_HostInputKey('J', 1);
    settle();
    s = get(0, &rc);
    CHECK(s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 0);
    xbox_HostInputStop();

    /* ---- a keyboard in a chosen slot ---- */
    xbox_HostInputDefaults(&cfg);
    cfg.keyboard = INPUT_HOST_KEYBOARD_SLOT3;
    keys_for_test(&cfg);
    CHECK(xbox_HostInputStart(&cfg) == 1);
    CHECK(xbox_HostInputKeyboardSlot() == 2 && xbox_HostInputPlayerCount() == 3);
    xbox_HostInputKey('J', 1);
    settle();
    s = get(2, &rc);
    CHECK(rc == ERROR_SUCCESS && s.Gamepad.bAnalogButtons[XBOX_BUTTON_A] == 255);
    xbox_HostInputStop();

    /* A pad that takes a player buzzes for a moment, then stops. */
    g_rumble_calls = 0;
    g_rumble_low = g_rumble_high = 0;
    xbox_HostInputDefaults(&cfg);
    CHECK(xbox_HostInputStart(&cfg) == 1);
    settle();
    CHECK(g_rumble_calls > 0 && g_rumble_low == 0xFFFF);
    Sleep(500);
    CHECK(g_rumble_low == 0 && g_rumble_high == 0);
    xbox_HostInputStop();

    SDL_CloseJoystick(pad1.joy);
    SDL_DetachVirtualJoystick(pad1.id);
    SDL_Quit();

    if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
    printf("input_host: all checks passed\n");
    return 0;
}
