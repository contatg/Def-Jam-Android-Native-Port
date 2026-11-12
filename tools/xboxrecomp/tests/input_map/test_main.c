#define _CRT_SECURE_NO_WARNINGS
/* input_map: key names, binding parse and format, key resolution, deadzone
 * arithmetic and pad remapping. */

#include "input_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } } while (0)
#define NEAR(a, b, tol) (abs((int)(a) - (int)(b)) <= (tol))

static void test_key_names(void)
{
    int c;
    char buf[64];
    CHECK(input_key_from_name("a") == 'A');
    CHECK(input_key_from_name("Z") == 'Z');
    CHECK(input_key_from_name("7") == '7');
    CHECK(input_key_from_name("F5") == 0x74);
    CHECK(input_key_from_name("f12") == 0x7B);
    CHECK(input_key_from_name("f13") == -1);
    CHECK(input_key_from_name("f05") == -1);
    CHECK(input_key_from_name("f0") == -1);
    CHECK(input_key_from_name("Numpad4") == 0x64);
    CHECK(input_key_from_name("numpad10") == -1);
    CHECK(input_key_from_name("SPACE") == 0x20);
    CHECK(input_key_from_name("Esc") == 0x1B);
    CHECK(input_key_from_name("mouse1") == 0x01);
    CHECK(input_key_from_name("Mouse3") == 0x04);
    CHECK(input_key_from_name("WheelUp") == INPUT_KEY_WHEEL_UP);
    CHECK(input_key_from_name("lshift") == 0xA0);
    CHECK(input_key_from_name("") == -1);
    CHECK(input_key_from_name("banana") == -1);
    CHECK(input_key_from_name("ab") == -1);
    /* Every code with a name reads back as itself. */
    for (c = 0; c < INPUT_KEY_COUNT; c++) {
        const char *n = input_key_name(c);
        if (!n) continue;
        snprintf(buf, sizeof(buf), "%s", n);
        CHECK(input_key_from_name(buf) == c);
    }
}

static void test_bindings(void)
{
    InputBindings b;
    char text[128];
    int k;
    memset(&b, 0, sizeof(b));

    CHECK(input_bindings_parse(&b, INPUT_A, "L, Space,  Mouse1 ") == 0);
    CHECK(b.count[INPUT_A] == 3);
    CHECK(b.key[INPUT_A][0] == 'L' && b.key[INPUT_A][1] == 0x20 && b.key[INPUT_A][2] == 0x01);
    CHECK(!strcmp(input_bindings_format(&b, INPUT_A, text, sizeof(text)), "L, space, mouse1"));

    /* Reading the formatted text back gives the same bindings. */
    k = input_bindings_parse(&b, INPUT_B, text);
    CHECK(k == 0 && b.count[INPUT_B] == 3 && b.key[INPUT_B][1] == 0x20);

    /* Parsing replaces; none and empty clear; duplicates are dropped. */
    CHECK(input_bindings_parse(&b, INPUT_A, "none") == 0 && b.count[INPUT_A] == 0);
    CHECK(!strcmp(input_bindings_format(&b, INPUT_A, text, sizeof(text)), "none"));
    CHECK(input_bindings_parse(&b, INPUT_A, "J, j, J") == 0 && b.count[INPUT_A] == 1);
    CHECK(input_bindings_parse(&b, INPUT_A, "") == 0 && b.count[INPUT_A] == 0);

    /* Unknown names are skipped and counted, known ones kept. */
    CHECK(input_bindings_parse(&b, INPUT_X, "K, banana, wheeldown, 99") == 2);
    CHECK(b.count[INPUT_X] == 2 && b.key[INPUT_X][1] == INPUT_KEY_WHEEL_DOWN);

    /* Four is the most; the fifth is counted as rejected. */
    CHECK(input_bindings_parse(&b, INPUT_Y, "A,B,C,D,E") == 1);
    CHECK(b.count[INPUT_Y] == 4);

    /* Control names are the setting keys. */
    CHECK(!strcmp(input_control_name(INPUT_LSTICK_UP), "left_stick_up"));
}

static void test_resolve(void)
{
    InputBindings b;
    uint8_t down[INPUT_KEY_COUNT];
    InputPad p;
    memset(&b, 0, sizeof(b));
    memset(down, 0, sizeof(down));
    input_bindings_parse(&b, INPUT_LSTICK_UP, "W");
    input_bindings_parse(&b, INPUT_LSTICK_DOWN, "S");
    input_bindings_parse(&b, INPUT_LSTICK_LEFT, "A");
    input_bindings_parse(&b, INPUT_LSTICK_RIGHT, "D");
    input_bindings_parse(&b, INPUT_RSTICK_LEFT, "Left");
    input_bindings_parse(&b, INPUT_A, "L, Space");
    input_bindings_parse(&b, INPUT_LEFT_TRIGGER, "Q");
    input_bindings_parse(&b, INPUT_START, "Enter");
    input_bindings_parse(&b, INPUT_DPAD_UP, "wheelup");

    input_keys_resolve(&b, down, &p);
    CHECK(p.buttons == 0 && p.lx == 0 && p.ly == 0 && p.rx == 0);

    down['W'] = 1;
    input_keys_resolve(&b, down, &p);
    CHECK(p.ly == 32767 && p.lx == 0);

    down['D'] = 1;                                   /* diagonal: not faster */
    input_keys_resolve(&b, down, &p);
    CHECK(p.lx == 23169 && p.ly == 23169);

    down['S'] = 1;                                   /* opposite keys cancel */
    input_keys_resolve(&b, down, &p);
    CHECK(p.ly == 0 && p.lx == 32767);

    down[0x25] = 1;
    input_keys_resolve(&b, down, &p);
    CHECK(p.rx == -32767 && p.ry == 0);

    down[0x20] = 1;                                  /* either bound key presses A */
    down['Q'] = 1;
    down[0x0D] = 1;
    down[INPUT_KEY_WHEEL_UP] = 1;
    input_keys_resolve(&b, down, &p);
    CHECK(p.analog[INPUT_ANALOG_A] == 255);
    CHECK(p.analog[INPUT_ANALOG_LTRIGGER] == 255);
    CHECK(p.analog[INPUT_ANALOG_B] == 0);
    CHECK(p.buttons == (INPUT_PAD_START | INPUT_PAD_DPAD_UP));
}

static void test_merge(void)
{
    InputPad a, b;
    memset(&a, 0, sizeof(a));
    memset(&b, 0, sizeof(b));
    a.buttons = INPUT_PAD_START;
    a.analog[0] = 255;
    a.lx = 1000;
    b.buttons = INPUT_PAD_BACK;
    b.analog[0] = 10;
    b.analog[7] = 90;
    b.lx = -20000;
    b.ry = 5;
    input_pad_merge(&a, &b);
    CHECK(a.buttons == (INPUT_PAD_START | INPUT_PAD_BACK));
    CHECK(a.analog[0] == 255 && a.analog[7] == 90);
    CHECK(a.lx == -20000);                           /* the larger deflection wins */
    CHECK(a.ry == 5);
}

static void test_deadzone(void)
{
    int16_t x, y;
    input_apply_stick(3000, 0, 15, &x, &y);          /* 15% is 4915 */
    CHECK(x == 0 && y == 0);
    input_apply_stick(3000, 3000, 15, &x, &y);       /* magnitude 4243: still inside */
    CHECK(x == 0 && y == 0);
    input_apply_stick(32767, 0, 15, &x, &y);         /* full stays full */
    CHECK(x == 32767 && y == 0);
    input_apply_stick(-32768, 0, 15, &x, &y);
    CHECK(x == -32767 && y == 0);
    input_apply_stick(16383, 0, 15, &x, &y);         /* (16383-4915)/(32767-4915) */
    CHECK(NEAR(x, 13491, 2) && y == 0);
    input_apply_stick(0, -16383, 15, &x, &y);
    CHECK(x == 0 && NEAR(y, -13491, 2));
    input_apply_stick(20000, 20000, 15, &x, &y);     /* direction preserved */
    CHECK(x == y && x > 0 && x < 32767);
    input_apply_stick(32767, 32767, 15, &x, &y);     /* beyond the circle clamps to it */
    CHECK(NEAR(x, 23170, 3) && NEAR(y, 23170, 3));
    input_apply_stick(500, 0, 0, &x, &y);            /* no deadzone passes through */
    CHECK(x == 500 && y == 0);
    input_apply_stick(0, 0, 15, &x, &y);
    CHECK(x == 0 && y == 0);
    input_apply_stick(32767, 0, 120, &x, &y);        /* absurd percentages are clamped */
    CHECK(x == 32767);

    CHECK(input_apply_trigger(0, 5) == 0);
    CHECK(input_apply_trigger(12, 5) == 0);          /* 5% of 255 is 12 */
    CHECK(input_apply_trigger(255, 5) == 255);
    CHECK(NEAR(input_apply_trigger(134, 5), 128, 1));
    CHECK(input_apply_trigger(100, 0) == 100);
    CHECK(input_apply_trigger(300, 5) == 255);
}

static void test_padmap(void)
{
    InputPadMap m;
    InputRaw r;
    InputPad p;
    int i;

    CHECK(input_source_from_name("south") == INPUT_SRC_SOUTH);
    CHECK(input_source_from_name("Left_Trigger") == INPUT_SRC_LEFT_TRIGGER);
    CHECK(input_source_from_name("a") == -1);
    for (i = 0; i < INPUT_SRC_COUNT; i++)
        CHECK(input_source_from_name(input_source_name(i)) == i);

    input_padmap_defaults(&m);
    memset(&r, 0, sizeof(r));
    input_pad_map(&m, &r, &p);
    CHECK(p.buttons == 0 && p.analog[0] == 0 && p.lx == 0);

    /* Identity: the bottom face button is A, the shoulders are Black and White. */
    r.src[INPUT_SRC_SOUTH] = 32767;
    r.src[INPUT_SRC_LEFT_SHOULDER] = 32767;
    r.src[INPUT_SRC_START] = 32767;
    r.src[INPUT_SRC_DPAD_LEFT] = 32767;
    r.src[INPUT_SRC_RIGHT_TRIGGER] = 32767;
    r.src[INPUT_SRC_LEFT_TRIGGER] = 128 << 7;
    input_pad_map(&m, &r, &p);
    CHECK(p.analog[INPUT_ANALOG_A] == 255 && p.analog[INPUT_ANALOG_B] == 0);
    CHECK(p.analog[INPUT_ANALOG_BLACK] == 255 && p.analog[INPUT_ANALOG_WHITE] == 0);
    CHECK(p.buttons == (INPUT_PAD_START | INPUT_PAD_DPAD_LEFT));
    CHECK(p.analog[INPUT_ANALOG_RTRIGGER] == 255);
    CHECK(NEAR(p.analog[INPUT_ANALOG_LTRIGGER], 128, 8));   /* analog, with the threshold applied */

    /* A button below the press threshold does not count; at it, does. */
    memset(&r, 0, sizeof(r));
    r.src[INPUT_SRC_EAST] = 16000;
    input_pad_map(&m, &r, &p);
    CHECK(p.analog[INPUT_ANALOG_B] == 0);
    r.src[INPUT_SRC_EAST] = 16384;
    input_pad_map(&m, &r, &p);
    CHECK(p.analog[INPUT_ANALOG_B] == 255);

    /* Remap: swap A and B, put Start on the touchpad, X on the left trigger. */
    m.source[INPUT_A] = INPUT_SRC_EAST;
    m.source[INPUT_B] = INPUT_SRC_SOUTH;
    m.source[INPUT_START] = INPUT_SRC_TOUCHPAD;
    m.source[INPUT_X] = INPUT_SRC_LEFT_TRIGGER;
    memset(&r, 0, sizeof(r));
    r.src[INPUT_SRC_SOUTH] = 32767;
    r.src[INPUT_SRC_TOUCHPAD] = 32767;
    r.src[INPUT_SRC_LEFT_TRIGGER] = 32767;
    input_pad_map(&m, &r, &p);
    CHECK(p.analog[INPUT_ANALOG_A] == 0 && p.analog[INPUT_ANALOG_B] == 255);
    CHECK(p.buttons == INPUT_PAD_START);
    CHECK(p.analog[INPUT_ANALOG_X] == 255);

    /* None leaves a control unbound. */
    input_padmap_defaults(&m);
    m.source[INPUT_A] = INPUT_SRC_NONE;
    memset(&r, 0, sizeof(r));
    r.src[INPUT_SRC_SOUTH] = 32767;
    input_pad_map(&m, &r, &p);
    CHECK(p.analog[INPUT_ANALOG_A] == 0);

    /* Sticks: pass through with the deadzone, swap, or switch off. */
    input_padmap_defaults(&m);
    memset(&r, 0, sizeof(r));
    r.lx = 32767; r.ly = 0; r.rx = 0; r.ry = -32767;
    input_pad_map(&m, &r, &p);
    CHECK(p.lx == 32767 && p.ry == -32767 && p.rx == 0);
    m.left_stick = INPUT_STICK_RIGHT;
    m.right_stick = INPUT_STICK_LEFT;
    input_pad_map(&m, &r, &p);
    CHECK(p.ly == -32767 && p.rx == 32767 && p.lx == 0);
    m.right_stick = INPUT_STICK_NONE;
    input_pad_map(&m, &r, &p);
    CHECK(p.rx == 0 && p.ry == 0);
    /* Drift inside the deadzone is ignored. */
    input_padmap_defaults(&m);
    r.lx = 2500; r.ly = -2500;
    input_pad_map(&m, &r, &p);
    CHECK(p.lx == 0 && p.ly == 0);
}

int main(void)
{
    test_key_names();
    test_bindings();
    test_resolve();
    test_merge();
    test_deadzone();
    test_padmap();
    if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
    printf("input_map: all checks passed\n");
    return 0;
}
