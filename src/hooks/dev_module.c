/*
 * The developer run module (CRunModuleAki), rebuilt from the host.
 *
 * The retail image keeps the module's methods -- Load 0x77C40 (makes the developer mode system,
 * subsystem 9), Update 0x77A40, Render 0x77B20, Unload 0x77DD0 and the constructor body 0x779F0 --
 * but not its virtual table, its constructor or its case in the run-module factory, so nothing
 * ever starts it. Here the table and the object are built in guest memory with the game's own
 * allocator and the module is queued the way the factory queues every module: stored as the run-
 * module manager's pending module (subsystem 0x17, +0x1C) with a switch requested (+0x24 = 1).
 * The manager then loads it on its loader thread, asks the current module whether it can leave,
 * stops and unloads that one, and starts this. docs/research/dev-module-revival.md has the map.
 *
 *   RECOMP_DEV_MODULE=1           queue it once the title screen is up (after the start-up movies)
 *   RECOMP_DEV_MODULE=<anchor>    ... once a front-end call containing <anchor> has run
 *   RECOMP_SCENE=<scene>          implies RECOMP_DEV_MODULE=1; after the scene the module is left
 *
 * Leaving requests the loader of the module that ran before (by its virtual table: the eight
 * module types and their loaders are tabled below), as the game's own flow does.
 *
 * Every method of ours is traced as [DEVMOD]. A test control: nothing changes unless set.
 */

#define _CRT_SECURE_NO_WARNINGS
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_call.h"
#include "recorder.h"
#include "dev_module.h"

#define MANAGER_SLOT    0x002FB1A4u     /* registry 0x2FB148, subsystem 0x17: CRunModuleMan */
#define FRONTEND_SLOT   0x002FB17Cu     /* registry 0x2FB148, subsystem 0x0D: the front end */
#define FE_SCREEN       0x9C            /* the screen the front end opens when it starts (sub_0006B000 sets it): an
                                           index into the name table at 0x2DFE08; 33 is "none" */
#define FE_SCREEN_NONE  33
#define FE_MAIN_MENU    15              /* intMain/mainMenu.swf (1 is the title, intMain/preStart.swf) */
#define MGR_CURRENT     0x18
#define MGR_PENDING     0x1C
#define MGR_STATE       0x24            /* 0 running, 1 requested, 2 loading, 3 loaded */
#define TAG_RUNMODULE   0x0029D4B4u     /* "RunModule", the factory's own allocation tag */
#define MODULE_SIZE     0x44            /* fields up to +0x40 (pause menu +0x3C, popup +0x40) */

extern void sub_00029680(void);         /* the game's allocator: cdecl (size, tag, flags) */
extern void sub_000779F0(void);         /* the module's constructor body (thiscall): pause menu, popup */
extern void sub_000773A0(void);         /* the manager's request(type) (thiscall, one argument) */
extern void sub_0001B1C0(void);         /* the task list's remove(task) (thiscall, one argument) */

/* The mode system's init (sub_0002C790) adds a static task to the task list (subsystem 1) with
 * sub_0001B170, which links it at the head without checking whether it is already there; the
 * cinema takes it out and puts it back, but nothing removes it when the module goes. Entered a
 * second time in one run, the module added it again: it then pointed at itself and the next walk
 * of the list never ended (the game froze at the second scene). EA's developers presumably never
 * left and re-entered this module; we take the task out when the module unloads. */
#define TASKS_SLOT      0x002FB14Cu     /* registry subsystem 1: the task list (head +4, next at node +8) */
#define DEV_TASK        0x002DBF28u

/* The factory's eight module types by virtual table, and the loader type that leads to each. */
static const uint32_t k_module_table[8] = { 0x0029D784u, 0x0029D5C4u, 0x0029D5E4u, 0x0029D604u,
                                            0x0029D664u, 0x0029D644u, 0x0029D624u, 0x0029D764u };
static const uint32_t k_loader_for[8] = { 9, 15, 10, 11, 13, 14, 12, 16 };

/* destructor, Load, Start, Update(dt), Render, CanLeave, Stop, Unload */
static const uint32_t k_slots[8] = {
    0x00076D30u,    /* the base class's destructor */
    0x00077C40u,    /* Load: CRunModuleAki::Init */
    0x00076EF0u,    /* Start: nothing (a bare ret) */
    0x00077A40u,    /* Update(dt) */
    0x00077B20u,    /* Render: returns 1 */
    0x00076EE0u,    /* CanLeave: returns 1 */
    0x00076EF0u,    /* Stop: nothing */
    0x00077DD0u,    /* Unload: undoes Init */
};

static volatile int s_mode = -1;        /* -1 env not read, 0 idle, 1 waiting for the anchor, 2 wanted, 3 queued */
static volatile int s_leave, s_for_scene, s_again;
static volatile int s_title_seen;       /* the start-up sequence is over: its own requests are done */
static char s_anchor[96];
static uint32_t s_table, s_object;
static int s_return_type = -1;          /* the module type that ran before ours */
static uint32_t s_fe_screen;            /* the front end's screen number then, given back when leaving */
static unsigned s_updates, s_renders;

static int module_type(uint32_t module)
{
    int i;
    if (!module) return -1;
    for (i = 0; i < 8; i++) if (MEM32(module) == k_module_table[i]) return i;
    return -1;
}

static int task_listed(uint32_t list, uint32_t task)
{
    uint32_t node = MEM32(list + 4);
    int n;
    for (n = 0; node && n < 4096; n++, node = MEM32(node + 8))
        if (node == task) return 1;
    return 0;
}

/* Take the developer task out of the task list if it is in it (the game's own remove). */
static void drop_dev_task(const char *when)
{
    uint32_t list = MEM32(TASKS_SLOT), task = DEV_TASK;
    if (!list || !task_listed(list, task)) return;
    guest_call(sub_0001B1C0, list, 1, &task);
    fprintf(stderr, "[DEVMOD] removed the developer task %08X from the task list (%s)%s\n", task, when,
            task_listed(list, task) ? "; it is STILL listed" : "");
}

static uint32_t guest_alloc(uint32_t size)
{
    uint32_t args[3];
    args[0] = size; args[1] = TAG_RUNMODULE; args[2] = 0;
    return guest_call(sub_00029680, 0, 3, args);
}

static void read_env(void)
{
    const char *v = getenv("RECOMP_DEV_MODULE"), *scene = getenv("RECOMP_SCENE");
    size_t i;
    s_mode = 0;
    if (scene && *scene) { s_for_scene = 1; if (!v || !*v) v = "1"; }
    if (!v || !*v || !strcmp(v, "0")) return;
    /* The title screen: the start-up sequence (logos, movies) is over, so no request of the game's
     * own replaces ours in the manager's single pending slot (it did at bootLoad). */
    snprintf(s_anchor, sizeof s_anchor, "%s", strcmp(v, "1") ? v : "getscreeninfo(intmain/prestart");
    for (i = 0; s_anchor[i]; i++) s_anchor[i] = (char)tolower((unsigned char)s_anchor[i]);
    s_mode = 1;
    fprintf(stderr, "[DEVMOD] armed: the developer module is queued after a front-end call containing \"%s\"\n",
            s_anchor);
}

int  dev_module_active(void) { uint32_t m = MEM32(MANAGER_SLOT); return s_object && m && MEM32(m + MGR_CURRENT) == s_object; }
void dev_module_request(int for_scene)
{
    if (s_mode < 0) read_env();
    s_leave = 0;
    if (dev_module_active() || s_mode == 3) return;
    s_for_scene = for_scene;
    s_mode = 2;
}
void dev_module_leave(void) { if (dev_module_active()) s_leave = 1; }
void dev_module_scene_ended(void) { if (s_for_scene) { s_for_scene = 0; dev_module_leave(); } }
void dev_module_again(void) { s_again = 1; }

void dev_module_after_call(const char *text)
{
    char low[160];
    size_t i;
#if defined(__ANDROID__)
    extern void defjam_android_note_frontend_call(const char *text);
    defjam_android_note_frontend_call(text);
#endif
    if (s_mode < 0) read_env();
    if (!text) return;
    for (i = 0; text[i] && i + 1 < sizeof low; i++) low[i] = (char)tolower((unsigned char)text[i]);
    low[i] = 0;
    if (strstr(low, "getscreeninfo(intmain/prestart")) s_title_seen = 1;
    if (s_mode != 1) return;
    if (strstr(low, s_anchor)) s_mode = 2;
}

/* The run-module manager's tick (thiscall, ecx = the manager), on the game's main thread. */
void dev_module_manager_enter(void)
{
    uint32_t mgr = g_ecx, obj;
    int i;

    if (s_mode < 0) read_env();
#if defined(__ANDROID__)
    {
        extern void defjam_android_note_module(int type);
        int type = -1;
        if (mgr && MEM32(mgr + MGR_STATE) == 0 && MEM32(mgr + MGR_PENDING) == 0)
            type = module_type(MEM32(mgr + MGR_CURRENT));
        defjam_android_note_module(type);
    }
#endif
    if (!mgr || MEM32(mgr + MGR_STATE) != 0 || MEM32(mgr + MGR_PENDING) != 0) return;
    if (s_leave && MEM32(mgr + MGR_CURRENT) == s_object) {
        /* Back to the front end: a game, Story or gym module cannot simply be re-entered (their own
         * flow sets up the match or the Story point first), so those return to the main menu too. */
        uint32_t type = k_loader_for[0];
        uint32_t fe = MEM32(FRONTEND_SLOT);
        const char *screen = getenv("RECOMP_DEV_MODULE_SCREEN");
        uint32_t want = screen && *screen ? (uint32_t)strtoul(screen, NULL, 0)
                      : s_return_type == 0 && s_fe_screen && s_fe_screen < FE_SCREEN_NONE ? s_fe_screen
                      : FE_MAIN_MENU;
        s_leave = 0;
        s_mode = s_again ? 2 : 0;           /* asked to come back (the next scene of a list) */
        s_for_scene = s_again;
        s_again = 0;
        /* As the game's own flow does before it requests the front end's loader (sub_000C1DF0). */
        if (fe && want) MEM32(fe + FE_SCREEN) = want;
        fprintf(stderr, "[DEVMOD] leaving: front-end screen %u, requesting loader type %u (the front end; type %d ran "
                "before)\n", fe ? MEM32(fe + FE_SCREEN) : 0, type, s_return_type);
        recorder_event_text("devmod", "state", "leaving");
        guest_call(sub_000773A0, mgr, 1, &type);
        return;
    }
    if (s_mode == 3 && MEM32(mgr + MGR_CURRENT) != s_object) {
        /* Queued but never made current: the game's own request took the single pending slot. */
        fprintf(stderr, "[DEVMOD] the queued module %08X was replaced by the game's own request before it ran\n",
                s_object);
        s_mode = 0;
        s_object = 0;
        return;
    }
    if (s_mode != 2 || !s_title_seen) return;     /* a request made earlier waits for the title screen */
    if (!s_table) {
        s_table = guest_alloc(sizeof k_slots);
        if (!s_table) { fprintf(stderr, "[DEVMOD] no memory for the table\n"); s_mode = 0; return; }
        for (i = 0; i < 8; i++) MEM32(s_table + (uint32_t)i * 4u) = k_slots[i];
    }
    obj = guest_alloc(MODULE_SIZE);
    if (!obj) { fprintf(stderr, "[DEVMOD] no memory for the module\n"); s_mode = 0; return; }
    for (i = 0; i < MODULE_SIZE; i += 4) MEM32(obj + (uint32_t)i) = 0;
    MEM32(obj) = s_table;
    guest_call(sub_000779F0, obj, 0, NULL);
    s_object = obj;
    s_return_type = module_type(MEM32(mgr + MGR_CURRENT));
    s_fe_screen = MEM32(FRONTEND_SLOT) ? MEM32(MEM32(FRONTEND_SLOT) + FE_SCREEN) : 0;
    MEM32(mgr + MGR_PENDING) = obj;
    MEM32(mgr + MGR_STATE) = 1;
    s_mode = 3;
    fprintf(stderr, "[DEVMOD] queued: module %08X, table %08X, pause menu %08X, popup %08X; current module %08X "
            "(type %d, table %08X), front-end screen %u\n", obj, s_table, MEM32(obj + 0x3C), MEM32(obj + 0x40),
            MEM32(mgr + MGR_CURRENT), s_return_type, MEM32(MEM32(mgr + MGR_CURRENT)), s_fe_screen);
    recorder_event_text("devmod", "state", "queued");
}

/* Traces: the methods of ours, entered with ecx = the module. */
void dev_module_trace(int slot)
{
    static const char *names[8] = { "destroy", "Load", "Start", "Update", "Render", "CanLeave", "Stop", "Unload" };
    if (!s_object || g_ecx != s_object) return;
    if (slot == 1) drop_dev_task("before Load");    /* in case anything left it there */
    if (slot == 7) drop_dev_task("at Unload");
    if (slot == 3 && MEM32(s_object + 0x14)) {     /* the mode system's state, whenever it changes */
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t state = MEM32(MEM32(s_object + 0x14) + 0x2A0);
        if (state != last) {
            static const char *modes[8] = { "debug menu", "game", "idle", "real time cinema", "stage viewer",
                                            "animation viewer", "create player", "crowd test or Blazin viewer" };
            fprintf(stderr, "[DEVMOD] mode state %u (%s)\n", state, state < 8 ? modes[state] : "?");
            last = state;
        }
    }
    if (slot == 3 && ++s_updates > 3 && s_updates % 600) return;
    if (slot == 4 && ++s_renders > 3 && s_renders % 600) return;
    if (slot == 3 || slot == 4) {
        fprintf(stderr, "[DEVMOD] %s #%u (mode system %08X, state %u)\n", names[slot], slot == 3 ? s_updates : s_renders,
                MEM32(s_object + 0x14), MEM32(s_object + 0x14) ? MEM32(MEM32(s_object + 0x14) + 0x2A0) : 0xFFFFFFFFu);
    } else {
        fprintf(stderr, "[DEVMOD] %s\n", names[slot]);
        recorder_event_text("devmod", "method", names[slot]);
    }
}
