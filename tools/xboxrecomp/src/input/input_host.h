/*
 * input_host -- the PC's pads, keyboard and mouse as Xbox controllers.
 *
 * The title polls up to four Xbox controllers. This layer decides which of the
 * host's devices is which one, and what each reads:
 *
 *   - Pads come from SDL3's gamepad layer when the toolkit is built with it
 *     (XBOXRECOMP_SDL3), which covers XInput, DualSense, Switch Pro and
 *     DirectInput pads under one set of names, and from XInput when it is not.
 *     Each is mapped through an InputPadMap (remapping, deadzones) and takes
 *     the lowest free slot in the order it appeared.
 *   - The keyboard and mouse are one more player, in a slot of their own, or
 *     merged into player 1, or off. The window that owns the keyboard feeds key
 *     and wheel events in; nothing here reads the keyboard itself.
 *   - Nothing is read while the game window lacks the focus: a pad in a
 *     background application does not play the game, and keys cannot stick.
 *   - Rumble goes the other way, to the pad in the slot the title vibrated.
 *
 * The devices are read on a thread of this layer's own (SDL wants one thread to
 * pump events), which keeps a snapshot per slot; xbox_HostInputGetState only
 * copies it, so any thread may poll.
 */
#ifndef XBOX_INPUT_HOST_H
#define XBOX_INPUT_HOST_H

#include "xinput_xbox.h"
#include "input_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How the keyboard takes part, for InputHostConfig.keyboard. */
enum {
    INPUT_HOST_KEYBOARD_OFF   = 0,
    INPUT_HOST_KEYBOARD_MERGE = 1,   /* adds to player 1's pad */
    INPUT_HOST_KEYBOARD_AUTO  = 2,   /* its own player, after the pads; player 1 with no pad */
    INPUT_HOST_KEYBOARD_SLOT1 = 3,   /* its own player in slot 1..4 */
    INPUT_HOST_KEYBOARD_SLOT2 = 4,
    INPUT_HOST_KEYBOARD_SLOT3 = 5,
    INPUT_HOST_KEYBOARD_SLOT4 = 6
};

typedef struct InputHostConfig {
    int  keyboard;              /* INPUT_HOST_KEYBOARD_* */
    int  players;               /* 0: as many as there are devices; 1-4: at least this many */
    int  use_sdl;               /* prefer SDL pads when the build has them */
    int  rumble_percent;        /* 0 turns rumble off; 100 is as the title asks */
    int  rumble_floor;          /* percent: a pulse weaker than this is raised to it (the title's are often faint) */
    int  rumble_min_ms;         /* a pulse is held at least this long, however briefly the title asks for it */
    int  rumble_on_connect;     /* buzz a pad for a moment when it takes a player, to show it works */
    int  ignore_focus;          /* test only: read devices without the window's focus */
    int  no_pads;               /* test only: open no physical pad, whatever is plugged in */
    int  virtual_pads;          /* test only: attach this many SDL virtual gamepads (0-4) */
    int  virtual_late_ms;       /* test only: attach one more virtual gamepad this long after start */
    int  virtual_chord_ms;      /* test only: this long after start virtual pad 1 holds both stick clicks for 1.8 s,
                                 * then presses B once (opens and closes a host menu) */
    InputPadMap   padmap;
    InputBindings keys;
} InputHostConfig;

/* The defaults the game's settings table matches: the default pad map, the
 * keyboard its own player, rumble at full strength, no key bound. */
void xbox_HostInputDefaults(InputHostConfig *cfg);

/* Open the devices and start the reader. Waits briefly for pads to show up so
 * the caller can size the emulated hub from what it finds. Returns the number
 * of pads opened. Call once, before the title starts polling. */
int xbox_HostInputStart(const InputHostConfig *cfg);

/* Called with the number of controllers a slot needs (slot + 1) whenever a pad
 * takes a slot, so the host program can plug one more into the emulated hub for a
 * pad that arrives while the game runs. Called from the reader thread. */
void xbox_HostInputOnPadSlot(void (*need_players)(int count));

/* Stop the reader, close the devices, stop any rumble. */
void xbox_HostInputStop(void);

/* Start has run and not been stopped. When this is false the toolkit's old
 * XInput path answers instead. */
int xbox_HostInputActive(void);

/* Change the pad map, bindings, rumble strength and focus rule while running.
 * The keyboard's slot and the player count are fixed at start. */
void xbox_HostInputConfigure(const InputHostConfig *cfg);

/* Test hook: hold the given controls on the keyboard player as if their first
 * bound key were down (`held` has INPUT_CONTROL_COUNT entries). Lets a scripted
 * run drive the whole keyboard path -- bindings, resolution, the slot, the USB
 * report -- with nobody at the keyboard. */
void xbox_HostInputScriptControls(const uint8_t *held);

/* Test hooks for virtual pads (InputHostConfig.virtual_pads): whether the pad in
 * `slot` is virtual, and hold the given controls on it (`held` has
 * INPUT_CONTROL_COUNT entries). Its rumble is logged as "[INPUT] virtual pad". */
int xbox_HostInputSlotIsVirtual(int slot);
void xbox_HostInputScriptVirtual(int slot, const uint8_t *held);

/* How many controllers the title should see on its hub: one per pad found, one
 * for a separate keyboard player, and at least `players`. Never below 1, never
 * above 4. Valid after xbox_HostInputStart. */
int xbox_HostInputPlayerCount(void);

/* The slot (0-3) the keyboard plays in, or -1 when it is off or merged. */
int xbox_HostInputKeyboardSlot(void);

/* The window reports its input here, from the window's own thread. `code` is a
 * Windows virtual-key code, including VK_LBUTTON and friends for the mouse. */
void xbox_HostInputKey(int code, int down);
void xbox_HostInputWheel(int notches);          /* positive is away from the user */
void xbox_HostInputFocus(int focused);          /* losing focus releases every key */

/* A host menu is open: the title polls every pad (and the keyboard player) at
 * rest, and keys and buttons are the menu's. The devices are still read, so the
 * menu can navigate with them (xbox_HostInputRawPad). Rumble stops. */
void xbox_HostInputSetUiActive(int active);
int xbox_HostInputUiActive(void);

/* Buzz the pad in `slot` for a moment (a menu's rumble test), even while a host menu is open. */
void xbox_HostInputRumbleTest(int slot);

/* What pad `slot` is doing now, before any mapping, for a menu to navigate and
 * to learn which control a player presses. Zero when there is no pad in the slot
 * or the window is unfocused. Returns 1 if a pad is there. */
int xbox_HostInputRawPad(int slot, InputRaw *raw);

/* The state the title polls. ERROR_DEVICE_NOT_CONNECTED when no device is in
 * the slot. While the window is unfocused a device reads as at rest. */
DWORD xbox_HostInputGetState(int slot, XBOX_INPUT_STATE *state);

/* Vibrate the pad in `slot` (0 stops it). Quietly succeeds with no pad. */
DWORD xbox_HostInputRumble(int slot, WORD low, WORD high);

/* What is in a slot: 0 empty, 1 a pad, 2 the keyboard, 3 both (merged). Copies
 * a short description into `name` if given. */
int xbox_HostInputSlotInfo(int slot, char *name, int size);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_HOST_H */
