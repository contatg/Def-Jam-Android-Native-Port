/*
 * Story scenes on demand, through the game's own developer cinema mode.
 *
 * The title kept EA's mode system (CModeSubsystem, registry subsystem 9, made by the
 * 3D module's init sub_00077C40). Its update sub_0002DFF0 (thiscall) is a state machine
 * on this+0x2A0: 0 the developer debug menu, 1 the game, 3 REAL TIME CINEMA, 4-7 the
 * viewers. sub_0002D4F0 starts the cinema: it takes the scene number from the mode's
 * settings (MEM32(this+0x2A8) + 0x18), names it through the table at 0x002DC018 (0 sc00
 * ... 22 sc21, 23 gf1a ... 37 gf3c), sets state 3 and has the cinema player at 0x3459A0
 * load it; state 3 plays it frame by frame, and sub_0002D570 ends it (state 0).
 *
 * So whenever the 3D module runs -- any fight, the crib, Story -- a scene can be played:
 * the hook at the update's entry sets the number, calls sub_0002D4F0 on the game's thread,
 * and when the scene has ended puts back the state that was running before. No profile,
 * no Story progress and no menus are involved.
 *
 *   RECOMP_SCENE=<name or number>    play it once the mode system has run half a second
 *                                    (RECOMP_SCENE_DELAY_FRAMES, default 30); the developer
 *                                    run module is started for it (dev_module.c) and left after
 *   scene_launcher_request(index)    the same from the overlay (Debug tab, Story scenes)
 *
 * The scene loader sub_00073FD0 is hooked too (scene_launcher_loader_enter): every scene
 * the game loads, by itself or asked for, is logged as [SCENE] and as a recorder `cinema`
 * event. A test control: nothing changes unless a scene is asked for.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_call.h"
#include "recorder.h"
#include "scene_launcher.h"
#include "dev_module.h"

#define SCENE_TABLE     0x002DC018u     /* scene number -> name, one guest pointer each */
#define MODE_ACTIVE     0x21            /* byte: the mode system is up */
#define MODE_STATE      0x2A0
#define MODE_SETTINGS   0x2A8           /* -> 15 words; the scene number is at +0x18 */
#define SETTING_SCENE   0x18
#define STATE_CINEMA    3

extern void sub_0002D4F0(void);         /* CModeSubsystem: start REAL TIME CINEMA (thiscall, no arguments) */

static volatile int s_request = -1;     /* asked for, any thread; -1 none */
static int s_queue[64], s_queued, s_next; /* RECOMP_SCENE=a,b,c: played in turn, the module re-entered for each */
static volatile int s_playing = -1;     /* the scene being played, -1 none */
static volatile unsigned s_updates;     /* mode updates seen: the 3D module is running while this moves */
static int s_saved_state, s_delay = -1;
static unsigned s_asked_at;

static void guest_text(uint32_t va, char *out, size_t size)
{
    size_t n = 0;
    while (va && n + 1 < size && MEM8(va + (uint32_t)n)) {
        out[n] = (char)MEM8(va + (uint32_t)n);
        n++;
    }
    out[n] = 0;
}

int scene_launcher_count(void)
{
    int i;
    for (i = 0; i < 64; i++) {
        uint32_t p = MEM32(SCENE_TABLE + (uint32_t)i * 4u);
        if (p < 0x10000u || p >= 0x04000000u || !MEM8(p)) break;   /* past the table: not a name pointer */
    }
    return i;
}

int scene_launcher_name(int index, char *out, size_t size)
{
    if (!out || !size) return 0;
    out[0] = 0;
    if (index < 0 || index >= scene_launcher_count()) return 0;
    guest_text(MEM32(SCENE_TABLE + (uint32_t)index * 4u), out, size);
    return out[0] != 0;
}

int scene_launcher_find(const char *name_or_number)
{
    char name[16], *end;
    long n;
    int i, count = scene_launcher_count();
    if (!name_or_number || !*name_or_number) return -1;
    n = strtol(name_or_number, &end, 10);
    if (*end == 0) return n >= 0 && n < count ? (int)n : -1;
    for (i = 0; i < count; i++)
        if (scene_launcher_name(i, name, sizeof name) && !strcmp(name, name_or_number)) return i;
    return -1;
}

void scene_launcher_request(int index) { s_request = index; s_asked_at = s_updates; }
int scene_launcher_playing(void) { return s_playing; }
unsigned scene_launcher_updates(void) { return s_updates; }

/* The scene loader's entry: name every scene the game loads. */
void scene_launcher_loader_enter(void)
{
    char name[32];
    guest_text(MEM32(g_esp + 4), name, sizeof name);
    if (!name[0]) return;
    fprintf(stderr, "[SCENE] loading %s\n", name);
    recorder_event_text("cinema", "scene", name);
}

/* The mode update's entry (ecx = the mode subsystem), once a frame while the 3D module runs. */
void scene_launcher_mode_enter(void)
{
    uint32_t mode = g_ecx, settings, state;
    char name[16];
    int index;

    s_updates++;
    if (s_delay < 0) {
        const char *want = getenv("RECOMP_SCENE"), *delay = getenv("RECOMP_SCENE_DELAY_FRAMES");
        s_delay = delay && *delay ? atoi(delay) : 30;
        while (want && *want && s_queued < 64) {
            char one[16];
            size_t n = strcspn(want, ",");
            snprintf(one, sizeof one, "%.*s", (int)(n < sizeof one - 1 ? n : sizeof one - 1), want);
            index = scene_launcher_find(one);
            if (index < 0) {
                char list[640] = "";
                size_t len = 0;
                int i;
                for (i = 0; i < scene_launcher_count() && scene_launcher_name(i, name, sizeof name); i++)
                    len += (size_t)snprintf(list + len, sizeof list - len, "%s%s", i ? " " : "", name);
                fprintf(stderr, "[SCENE] RECOMP_SCENE: %s is not a scene of this game; known: %s\n", one, list);
            } else {
                s_queue[s_queued++] = index;
            }
            want += n + (want[n] == ',');
        }
        if (s_queued) scene_launcher_request(s_queue[s_next++]);
    }
    if (!mode || !MEM8(mode + MODE_ACTIVE)) return;
    state = MEM32(mode + MODE_STATE);

    if (s_playing >= 0) {
        if (state == STATE_CINEMA) return;
        /* sub_0002D570 ended the scene and left the debug menu's state: back to what ran before. */
        MEM32(mode + MODE_STATE) = (uint32_t)s_saved_state;
        scene_launcher_name(s_playing, name, sizeof name);
        fprintf(stderr, "[SCENE] %s ended; mode state %d again\n", name, s_saved_state);
        recorder_event_text("cinema", "ended", name);
        s_playing = -1;
        if (s_next < s_queued) {            /* the next of RECOMP_SCENE's list: leave, then come back for it */
            scene_launcher_request(s_queue[s_next++]);
            dev_module_again();
        }
        dev_module_scene_ended();
        return;
    }
    index = s_request;
    if (index < 0 || s_updates - s_asked_at < (unsigned)s_delay || state == STATE_CINEMA) return;
    s_request = -1;
    settings = MEM32(mode + MODE_SETTINGS);
    if (!settings || !scene_launcher_name(index, name, sizeof name)) return;

    s_saved_state = (int)state;
    MEM32(settings + SETTING_SCENE) = (uint32_t)index;
    guest_call(sub_0002D4F0, mode, 0, NULL);
    if (MEM32(mode + MODE_STATE) != STATE_CINEMA) {
        fprintf(stderr, "[SCENE] %s did not start (mode state %u)\n", name, MEM32(mode + MODE_STATE));
        return;
    }
    s_playing = index;
    fprintf(stderr, "[SCENE] playing %s (scene %d) in the game's cinema mode; mode state was %d\n",
            name, index, s_saved_state);
    recorder_event_text("cinema", "play", name);
}
