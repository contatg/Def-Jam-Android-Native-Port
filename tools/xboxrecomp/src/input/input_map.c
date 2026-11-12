/* Host input to Xbox controller rules. See input_map.h. */

#include "input_map.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- control names ------------------------------------------------------- */

static const char *const k_control_names[INPUT_CONTROL_COUNT] = {
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "start", "back", "left_thumb", "right_thumb",
    "a", "b", "x", "y", "black", "white",
    "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

const char *input_control_name(int control)
{
    return control >= 0 && control < INPUT_CONTROL_COUNT ? k_control_names[control] : NULL;
}

static int equal_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

/* ---- merging ------------------------------------------------------------- */

static long stick_mag2(int x, int y) { return (long)x * x + (long)y * y; }

void input_pad_merge(InputPad *dst, const InputPad *src)
{
    int i;
    dst->buttons |= src->buttons;
    for (i = 0; i < 8; i++)
        if (src->analog[i] > dst->analog[i]) dst->analog[i] = src->analog[i];
    if (stick_mag2(src->lx, src->ly) > stick_mag2(dst->lx, dst->ly)) {
        dst->lx = src->lx; dst->ly = src->ly;
    }
    if (stick_mag2(src->rx, src->ry) > stick_mag2(dst->rx, dst->ry)) {
        dst->rx = src->rx; dst->ry = src->ry;
    }
}

/* ---- keys ---------------------------------------------------------------- */

typedef struct KeyName { const char *name; int code; } KeyName;

/* The first name listed for a code is the one written back. Letters, digits,
 * function keys and the numeric pad's digits are generated rather than listed. */
static const KeyName k_keys[] = {
    { "mouse1", 0x01 }, { "mouse2", 0x02 }, { "mouse3", 0x04 },
    { "mouse4", 0x05 }, { "mouse5", 0x06 },
    { "wheelup", INPUT_KEY_WHEEL_UP }, { "wheeldown", INPUT_KEY_WHEEL_DOWN },
    { "backspace", 0x08 }, { "tab", 0x09 }, { "enter", 0x0D }, { "return", 0x0D },
    { "shift", 0x10 }, { "ctrl", 0x11 }, { "control", 0x11 }, { "alt", 0x12 },
    { "pause", 0x13 }, { "capslock", 0x14 },
    { "escape", 0x1B }, { "esc", 0x1B }, { "space", 0x20 },
    { "pageup", 0x21 }, { "pagedown", 0x22 }, { "end", 0x23 }, { "home", 0x24 },
    { "left", 0x25 }, { "up", 0x26 }, { "right", 0x27 }, { "down", 0x28 },
    { "insert", 0x2D }, { "delete", 0x2E },
    { "numpadmultiply", 0x6A }, { "numpadadd", 0x6B }, { "numpadsubtract", 0x6D },
    { "numpaddecimal", 0x6E }, { "numpaddivide", 0x6F },
    { "lshift", 0xA0 }, { "rshift", 0xA1 }, { "lctrl", 0xA2 }, { "rctrl", 0xA3 },
    { "lalt", 0xA4 }, { "ralt", 0xA5 },
    { "semicolon", 0xBA }, { "equals", 0xBB }, { "comma", 0xBC }, { "minus", 0xBD },
    { "period", 0xBE }, { "slash", 0xBF }, { "grave", 0xC0 },
    { "leftbracket", 0xDB }, { "backslash", 0xDC }, { "rightbracket", 0xDD },
    { "quote", 0xDE },
};
#define KEY_TABLE_COUNT ((int)(sizeof(k_keys) / sizeof(k_keys[0])))

/* "f5" or "numpad3": a fixed prefix and a number with no leading zero. */
static int numbered(const char *name, const char *prefix, int lo, int hi)
{
    size_t n = strlen(prefix);
    char tail[8];
    int v;
    if (strlen(name) <= n || strlen(name) > n + 2) return -1;
    {
        size_t i;
        for (i = 0; i < n; i++)
            if (tolower((unsigned char)name[i]) != prefix[i]) return -1;
    }
    v = atoi(name + n);
    snprintf(tail, sizeof(tail), "%d", v);
    if (strcmp(tail, name + n) || v < lo || v > hi) return -1;
    return v;
}

int input_key_from_name(const char *name)
{
    int i, v;
    if (!name || !*name) return -1;
    if (name[1] == 0) {
        char c = (char)toupper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
        return -1;
    }
    v = numbered(name, "f", 1, 12);
    if (v >= 0) return 0x70 + v - 1;
    v = numbered(name, "numpad", 0, 9);
    if (v >= 0) return 0x60 + v;
    for (i = 0; i < KEY_TABLE_COUNT; i++)
        if (equal_nocase(name, k_keys[i].name)) return k_keys[i].code;
    return -1;
}

const char *input_key_name(int code)
{
    static char generated[4][12];
    static int next;
    char *g;
    int i;
    if (code >= 'A' && code <= 'Z') {
        g = generated[next++ & 3];
        snprintf(g, sizeof(generated[0]), "%c", code);
        return g;
    }
    if (code >= '0' && code <= '9') {
        g = generated[next++ & 3];
        snprintf(g, sizeof(generated[0]), "%c", code);
        return g;
    }
    if (code >= 0x70 && code <= 0x7B) {
        g = generated[next++ & 3];
        snprintf(g, sizeof(generated[0]), "F%d", code - 0x70 + 1);
        return g;
    }
    if (code >= 0x60 && code <= 0x69) {
        g = generated[next++ & 3];
        snprintf(g, sizeof(generated[0]), "Numpad%d", code - 0x60);
        return g;
    }
    for (i = 0; i < KEY_TABLE_COUNT; i++)
        if (k_keys[i].code == code) return k_keys[i].name;
    return NULL;
}

int input_bindings_parse(InputBindings *b, int control, const char *text)
{
    int rejected = 0;
    char copy[256], *tok, *next;

    if (!b || control < 0 || control >= INPUT_CONTROL_COUNT) return 0;
    b->count[control] = 0;
    if (!text) return 0;
    snprintf(copy, sizeof(copy), "%s", text);
    for (tok = copy; tok; tok = next) {
        char *end;
        int code, j, dup = 0;
        next = strchr(tok, ',');
        if (next) *next++ = 0;
        while (*tok && isspace((unsigned char)*tok)) tok++;
        end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) *--end = 0;
        if (!*tok || equal_nocase(tok, "none")) continue;
        code = input_key_from_name(tok);
        if (code < 0) { rejected++; continue; }
        for (j = 0; j < b->count[control]; j++)
            if (b->key[control][j] == code) dup = 1;
        if (dup) continue;
        if (b->count[control] < INPUT_BINDINGS_PER_CONTROL)
            b->key[control][b->count[control]++] = (uint16_t)code;
        else
            rejected++;
    }
    return rejected;
}

const char *input_bindings_format(const InputBindings *b, int control, char *buf, int size)
{
    int i, used = 0;
    if (!buf || size <= 0) return buf;
    buf[0] = 0;
    if (!b || control < 0 || control >= INPUT_CONTROL_COUNT || !b->count[control]) {
        snprintf(buf, (size_t)size, "none");
        return buf;
    }
    for (i = 0; i < b->count[control]; i++) {
        const char *n = input_key_name(b->key[control][i]);
        int w = snprintf(buf + used, (size_t)(size - used), "%s%s", i ? ", " : "",
                         n ? n : "?");
        if (w < 0 || w >= size - used) break;
        used += w;
    }
    return buf;
}

static int control_held(const InputBindings *b, const uint8_t *down, int control)
{
    int i;
    for (i = 0; i < b->count[control]; i++) {
        int k = b->key[control][i];
        if (k < INPUT_KEY_COUNT && down[k]) return 1;
    }
    return 0;
}

/* A stick from four keys: a unit-circle deflection, so a diagonal is as fast as
 * a straight line (32767 * 0.7071 on each axis). */
static void keys_stick(int right, int left_k, int up, int down_k, int16_t *ox, int16_t *oy)
{
    int x = (right ? 1 : 0) - (left_k ? 1 : 0);
    int y = (up ? 1 : 0) - (down_k ? 1 : 0);
    if (x && y) {
        *ox = (int16_t)(x * 23169);
        *oy = (int16_t)(y * 23169);
    } else {
        *ox = (int16_t)(x * 32767);
        *oy = (int16_t)(y * 32767);
    }
}

void input_keys_resolve(const InputBindings *b, const uint8_t *down, InputPad *out)
{
    static const struct { int control; uint16_t bit; } digital[8] = {
        { INPUT_DPAD_UP, INPUT_PAD_DPAD_UP }, { INPUT_DPAD_DOWN, INPUT_PAD_DPAD_DOWN },
        { INPUT_DPAD_LEFT, INPUT_PAD_DPAD_LEFT }, { INPUT_DPAD_RIGHT, INPUT_PAD_DPAD_RIGHT },
        { INPUT_START, INPUT_PAD_START }, { INPUT_BACK, INPUT_PAD_BACK },
        { INPUT_LEFT_THUMB, INPUT_PAD_LEFT_THUMB }, { INPUT_RIGHT_THUMB, INPUT_PAD_RIGHT_THUMB },
    };
    /* Analog button slot for each control from INPUT_A on. */
    static const uint8_t analog_slot[] = {
        INPUT_ANALOG_A, INPUT_ANALOG_B, INPUT_ANALOG_X, INPUT_ANALOG_Y,
        INPUT_ANALOG_BLACK, INPUT_ANALOG_WHITE, INPUT_ANALOG_LTRIGGER, INPUT_ANALOG_RTRIGGER,
    };
    int i;

    memset(out, 0, sizeof(*out));
    for (i = 0; i < 8; i++)
        if (control_held(b, down, digital[i].control)) out->buttons |= digital[i].bit;
    /* Analog on the console: a key is 255, not a flag, or a title that reads
     * the value as a pressure never sees a press. */
    for (i = 0; i < 8; i++)
        if (control_held(b, down, INPUT_A + i)) out->analog[analog_slot[i]] = 255;
    keys_stick(control_held(b, down, INPUT_LSTICK_RIGHT), control_held(b, down, INPUT_LSTICK_LEFT),
               control_held(b, down, INPUT_LSTICK_UP), control_held(b, down, INPUT_LSTICK_DOWN),
               &out->lx, &out->ly);
    keys_stick(control_held(b, down, INPUT_RSTICK_RIGHT), control_held(b, down, INPUT_RSTICK_LEFT),
               control_held(b, down, INPUT_RSTICK_UP), control_held(b, down, INPUT_RSTICK_DOWN),
               &out->rx, &out->ry);
}

/* ---- pad sources ---------------------------------------------------------- */

static const char *const k_source_names[INPUT_SRC_COUNT] = {
    "none",
    "south", "east", "west", "north",
    "back", "guide", "start",
    "left_stick_click", "right_stick_click",
    "left_shoulder", "right_shoulder",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "misc1", "touchpad",
    "paddle1", "paddle2", "paddle3", "paddle4",
    "left_trigger", "right_trigger",
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right",
};

const char *input_source_name(int source)
{
    return source >= 0 && source < INPUT_SRC_COUNT ? k_source_names[source] : NULL;
}

int input_source_from_name(const char *name)
{
    int i;
    if (!name) return -1;
    for (i = 0; i < INPUT_SRC_COUNT; i++)
        if (equal_nocase(name, k_source_names[i])) return i;
    return -1;
}

void input_padmap_defaults(InputPadMap *m)
{
    static const uint8_t identity[INPUT_PAD_CONTROL_COUNT] = {
        INPUT_SRC_DPAD_UP, INPUT_SRC_DPAD_DOWN, INPUT_SRC_DPAD_LEFT, INPUT_SRC_DPAD_RIGHT,
        INPUT_SRC_START, INPUT_SRC_BACK,
        INPUT_SRC_LEFT_STICK_CLICK, INPUT_SRC_RIGHT_STICK_CLICK,
        INPUT_SRC_SOUTH, INPUT_SRC_EAST, INPUT_SRC_WEST, INPUT_SRC_NORTH,
        INPUT_SRC_LEFT_SHOULDER, INPUT_SRC_RIGHT_SHOULDER,
        INPUT_SRC_LEFT_TRIGGER, INPUT_SRC_RIGHT_TRIGGER,
    };
    memcpy(m->source, identity, sizeof(identity));
    m->left_stick = INPUT_STICK_LEFT;
    m->right_stick = INPUT_STICK_RIGHT;
    m->deadzone_left = 15;
    m->deadzone_right = 15;
    m->trigger_threshold = 5;
    m->button_threshold = 50;
}

/* ---- deadzones ------------------------------------------------------------ */

void input_apply_stick(int x, int y, int deadzone_pct, int16_t *ox, int16_t *oy)
{
    double dz, mag, scale;
    if (deadzone_pct < 0) deadzone_pct = 0;
    if (deadzone_pct > 95) deadzone_pct = 95;
    dz = 32767.0 * deadzone_pct / 100.0;
    mag = sqrt((double)x * x + (double)y * y);
    if (mag <= dz || mag == 0.0) {
        *ox = 0; *oy = 0;
        return;
    }
    scale = (mag - dz) / (32767.0 - dz);
    if (scale > 1.0) scale = 1.0;
    {
        double fx = x / mag * scale * 32767.0, fy = y / mag * scale * 32767.0;
        *ox = (int16_t)(fx >= 0 ? fx + 0.5 : fx - 0.5);
        *oy = (int16_t)(fy >= 0 ? fy + 0.5 : fy - 0.5);
    }
}

uint8_t input_apply_trigger(int value, int threshold_pct)
{
    int thr;
    if (value < 0) value = 0;
    if (value > 255) value = 255;
    if (threshold_pct < 0) threshold_pct = 0;
    if (threshold_pct > 95) threshold_pct = 95;
    thr = 255 * threshold_pct / 100;
    if (value <= thr) return 0;
    return (uint8_t)((value - thr) * 255 / (255 - thr));
}

/* ---- pad mapping ---------------------------------------------------------- */

static int raw_value(const InputRaw *r, int source)
{
    return source > INPUT_SRC_NONE && source < INPUT_SRC_COUNT ? r->src[source] : 0;
}

void input_pad_map(const InputPadMap *m, const InputRaw *raw, InputPad *out)
{
    static const uint16_t digital_bit[8] = {
        INPUT_PAD_DPAD_UP, INPUT_PAD_DPAD_DOWN, INPUT_PAD_DPAD_LEFT, INPUT_PAD_DPAD_RIGHT,
        INPUT_PAD_START, INPUT_PAD_BACK, INPUT_PAD_LEFT_THUMB, INPUT_PAD_RIGHT_THUMB,
    };
    static const uint8_t analog_slot[8] = {
        INPUT_ANALOG_A, INPUT_ANALOG_B, INPUT_ANALOG_X, INPUT_ANALOG_Y,
        INPUT_ANALOG_BLACK, INPUT_ANALOG_WHITE, INPUT_ANALOG_LTRIGGER, INPUT_ANALOG_RTRIGGER,
    };
    int press_at = 32767 * m->button_threshold / 100;
    int i;

    memset(out, 0, sizeof(*out));
    if (press_at < 1) press_at = 1;
    for (i = 0; i < 8; i++)
        if (raw_value(raw, m->source[i]) >= press_at) out->buttons |= digital_bit[i];
    /* The face buttons and shoulders are analog on the console but binary
     * here: pressed is 255. The triggers keep the pad's own travel. */
    for (i = 0; i < 6; i++)
        if (raw_value(raw, m->source[INPUT_A + i]) >= press_at)
            out->analog[analog_slot[i]] = 255;
    for (i = 6; i < 8; i++) {
        int v = raw_value(raw, m->source[INPUT_A + i]) >> 7;
        out->analog[analog_slot[i]] = input_apply_trigger(v > 255 ? 255 : v, m->trigger_threshold);
    }
    {
        int sx = 0, sy = 0;
        switch (m->left_stick) {
        case INPUT_STICK_LEFT:  sx = raw->lx; sy = raw->ly; break;
        case INPUT_STICK_RIGHT: sx = raw->rx; sy = raw->ry; break;
        default: break;
        }
        input_apply_stick(sx, sy, m->deadzone_left, &out->lx, &out->ly);
        sx = sy = 0;
        switch (m->right_stick) {
        case INPUT_STICK_LEFT:  sx = raw->lx; sy = raw->ly; break;
        case INPUT_STICK_RIGHT: sx = raw->rx; sy = raw->ry; break;
        default: break;
        }
        input_apply_stick(sx, sy, m->deadzone_right, &out->rx, &out->ry);
    }
}
