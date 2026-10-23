/*
 * input_map -- what a host key, mouse button or pad control means to an Xbox
 * controller.
 *
 * Pure arithmetic and text: no Windows, no SDL, no globals. The host layer
 * (input_host.c) reads the devices and hands the raw values to this; keeping
 * the rules here is what lets a test check binding resolution and deadzones
 * without a pad on the desk.
 *
 * Three pieces:
 *
 *   Keyboard and mouse bindings. Each Xbox control has up to four host inputs
 *   bound to it, written as a comma separated list of names ("L, Space,
 *   Mouse1"). A key is a Windows virtual-key code (0-255, mouse buttons
 *   included as VK_LBUTTON and friends) or one of two wheel pseudo keys.
 *
 *   Pad remapping. Each of the pad's sixteen digital and trigger controls
 *   takes its value from one named physical control (the names are SDL's
 *   positional ones: south is the bottom face button, an Xbox A, a PlayStation
 *   cross), and each stick can come from either physical stick or from none.
 *
 *   Deadzones. A radial deadzone for sticks that rescales so the stick still
 *   reaches full deflection, and a threshold for triggers.
 */
#ifndef XBOX_INPUT_MAP_H
#define XBOX_INPUT_MAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Xbox controller's inputs, in the order settings list them. The first
 * sixteen are what a pad's physical controls can be remapped onto. */
typedef enum InputControl {
    INPUT_DPAD_UP, INPUT_DPAD_DOWN, INPUT_DPAD_LEFT, INPUT_DPAD_RIGHT,
    INPUT_START, INPUT_BACK, INPUT_LEFT_THUMB, INPUT_RIGHT_THUMB,
    INPUT_A, INPUT_B, INPUT_X, INPUT_Y, INPUT_BLACK, INPUT_WHITE,
    INPUT_LEFT_TRIGGER, INPUT_RIGHT_TRIGGER,
    /* Keyboard only: the sticks as four keys each. */
    INPUT_LSTICK_UP, INPUT_LSTICK_DOWN, INPUT_LSTICK_LEFT, INPUT_LSTICK_RIGHT,
    INPUT_RSTICK_UP, INPUT_RSTICK_DOWN, INPUT_RSTICK_LEFT, INPUT_RSTICK_RIGHT,
    INPUT_CONTROL_COUNT
} InputControl;
#define INPUT_PAD_CONTROL_COUNT 16

/* The setting key for a control ("dpad_up", "left_stick_left"), or NULL. */
const char *input_control_name(int control);

/* An Xbox controller's state as the title sees it. Same layout as the USB
 * report: digital buttons in the low byte, eight analog buttons (A B X Y Black
 * White, left and right trigger), and the sticks with Y positive upwards. */
typedef struct InputPad {
    uint16_t buttons;               /* INPUT_PAD_* bits */
    uint8_t  analog[8];             /* INPUT_ANALOG_* */
    int16_t  lx, ly, rx, ry;
} InputPad;

#define INPUT_PAD_DPAD_UP     0x0001
#define INPUT_PAD_DPAD_DOWN   0x0002
#define INPUT_PAD_DPAD_LEFT   0x0004
#define INPUT_PAD_DPAD_RIGHT  0x0008
#define INPUT_PAD_START       0x0010
#define INPUT_PAD_BACK        0x0020
#define INPUT_PAD_LEFT_THUMB  0x0040
#define INPUT_PAD_RIGHT_THUMB 0x0080

#define INPUT_ANALOG_A        0
#define INPUT_ANALOG_B        1
#define INPUT_ANALOG_X        2
#define INPUT_ANALOG_Y        3
#define INPUT_ANALOG_BLACK    4
#define INPUT_ANALOG_WHITE    5
#define INPUT_ANALOG_LTRIGGER 6
#define INPUT_ANALOG_RTRIGGER 7

/* Add `src` to `dst`: buttons are ORed, analog buttons take the larger, and a
 * stick takes whichever of the two is deflected further. */
void input_pad_merge(InputPad *dst, const InputPad *src);

/* ---- keyboard and mouse -------------------------------------------------- */

#define INPUT_KEY_WHEEL_UP    256   /* a notch of the wheel, as a short press */
#define INPUT_KEY_WHEEL_DOWN  257
#define INPUT_KEY_COUNT       258
#define INPUT_BINDINGS_PER_CONTROL 4

typedef struct InputBindings {
    uint16_t key[INPUT_CONTROL_COUNT][INPUT_BINDINGS_PER_CONTROL];
    uint8_t  count[INPUT_CONTROL_COUNT];
} InputBindings;

/* Code for a key name, case-insensitive ("a", "space", "numpad4", "mouse1",
 * "wheelup", "lshift"), or -1 if there is no such key. */
int input_key_from_name(const char *name);

/* The canonical name for a code, or NULL. */
const char *input_key_name(int code);

/* Parse "L, Space" into `control`'s bindings, replacing them. "none" or an
 * empty string clears it. Names that are not keys are skipped and counted in
 * the return value; at most four keys are kept. Returns the number of names
 * rejected, so 0 means the whole text was understood. */
int input_bindings_parse(InputBindings *b, int control, const char *text);

/* The bindings of `control` as text that input_bindings_parse reads back. */
const char *input_bindings_format(const InputBindings *b, int control,
                                  char *buf, int size);

/* The pad a set of held keys makes. `down` has INPUT_KEY_COUNT entries, non-zero
 * for held. A stick built from keys is a full-deflection circle, so a diagonal
 * is not faster than a straight line. */
void input_keys_resolve(const InputBindings *b, const uint8_t *down, InputPad *out);

/* ---- pad remapping and deadzones ------------------------------------------ */

/* The physical controls a pad has, SDL's positional names. */
typedef enum InputSource {
    INPUT_SRC_NONE,
    INPUT_SRC_SOUTH, INPUT_SRC_EAST, INPUT_SRC_WEST, INPUT_SRC_NORTH,
    INPUT_SRC_BACK, INPUT_SRC_GUIDE, INPUT_SRC_START,
    INPUT_SRC_LEFT_STICK_CLICK, INPUT_SRC_RIGHT_STICK_CLICK,
    INPUT_SRC_LEFT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER,
    INPUT_SRC_DPAD_UP, INPUT_SRC_DPAD_DOWN, INPUT_SRC_DPAD_LEFT, INPUT_SRC_DPAD_RIGHT,
    INPUT_SRC_MISC1, INPUT_SRC_TOUCHPAD,
    INPUT_SRC_PADDLE1, INPUT_SRC_PADDLE2, INPUT_SRC_PADDLE3, INPUT_SRC_PADDLE4,
    INPUT_SRC_LEFT_TRIGGER, INPUT_SRC_RIGHT_TRIGGER,
    /* A stick pushed one way, 0 to full. */
    INPUT_SRC_LSTICK_UP, INPUT_SRC_LSTICK_DOWN, INPUT_SRC_LSTICK_LEFT, INPUT_SRC_LSTICK_RIGHT,
    INPUT_SRC_RSTICK_UP, INPUT_SRC_RSTICK_DOWN, INPUT_SRC_RSTICK_LEFT, INPUT_SRC_RSTICK_RIGHT,
    INPUT_SRC_COUNT
} InputSource;

const char *input_source_name(int source);
int input_source_from_name(const char *name);      /* -1 if unknown */

/* What the host read from a pad before any mapping. Every source is 0 to
 * 32767 (a button is 0 or 32767); the sticks are signed, Y positive upwards. */
typedef struct InputRaw {
    int16_t src[INPUT_SRC_COUNT];
    int16_t lx, ly, rx, ry;
} InputRaw;

enum { INPUT_STICK_LEFT, INPUT_STICK_RIGHT, INPUT_STICK_NONE };

typedef struct InputPadMap {
    uint8_t source[INPUT_PAD_CONTROL_COUNT];   /* InputSource for each control */
    uint8_t left_stick;                        /* INPUT_STICK_*: which physical stick */
    uint8_t right_stick;
    uint8_t deadzone_left;                     /* percent, 0-90 */
    uint8_t deadzone_right;
    uint8_t trigger_threshold;                 /* percent, 0-90 */
    uint8_t button_threshold;                  /* percent a trigger/stick must reach to count as a button */
} InputPadMap;

/* The identity mapping: a pad as the Xbox pad it is shaped like. Deadzones 15%,
 * trigger threshold 5%, an axis counts as a button at 50%. */
void input_padmap_defaults(InputPadMap *m);

/* Apply `m` to a raw pad. */
void input_pad_map(const InputPadMap *m, const InputRaw *raw, InputPad *out);

/* A radial deadzone: a stick inside `deadzone_pct` of full deflection reads as
 * centred, and the range beyond it is stretched so full deflection is still
 * full. The direction is preserved. */
void input_apply_stick(int x, int y, int deadzone_pct, int16_t *ox, int16_t *oy);

/* A trigger value (0-255) below `threshold_pct` of the range reads 0; the rest
 * is stretched to 0-255. */
uint8_t input_apply_trigger(int value, int threshold_pct);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_INPUT_MAP_H */
