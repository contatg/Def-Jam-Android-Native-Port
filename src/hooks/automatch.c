#if defined(__ANDROID__)
/* Adapted from sweeax-exe/ffny-online@5664adca (MIT). */
/* Starting a match without the menus. See automatch.h and docs/research/online-match-setup.md.
 *
 * The front end hands the game a match through FuncCall strings (Game.SetMatchType,
 * Controller.ControllerSetup, Game.ChooseVenue, Game.StartGame, ...). This replays the sequence a
 * two-player Versus run logs, with the requested values, on the game thread (the FuncCall hook),
 * once the main menu, or the battle match-type screen a match's summary leads back to, has
 * settled: no MemCard call for a while, which is when the boot-time save checks are done.
 *
 * Tests: RECOMP_AUTOMATCH="type=0,venue=1,f0=55,p0=0,f1=22,p1=-1" (pads: -1 is the CPU).
 * RECOMP_FE_RESULTS=1 prints what the roster, venue and match-type queries return. */
#define _CRT_SECURE_NO_WARNINGS
#include "host.h"
#include <ctype.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "recomp/gen/recomp_types.h"
#include "automatch.h"
#include "netplay.h"
#include "android_netplay.h"


extern void xbox_NoteAnchorEvent(const char *text);

/* The idle main menu makes only a handful of FuncCalls (its music and sounds), so the sequence
 * runs at the menu music's start, or after a few calls with no MemCard call in between. */
#define SETTLE_CALLS 4

static AutoMatch g_req;
static volatile LONG g_armed;
static _Atomic int g_on_menu, g_running; /* published to the Android UI/network worker */
static int g_quiet, g_ticks, g_env_read, g_results = -1;

/* Android calls Online.Tick() from the existing guest screen-manager hook: this many
 * ticks with no MemCard call in between count as settled too, so a match agreed while the
 * menu sits idle still starts. */
#define SETTLE_TICKS 45

static char g_fe_queue[4][160];
static volatile LONG g_fe_count;
static CRITICAL_SECTION g_fe_cs;
static INIT_ONCE g_fe_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK fe_init(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&g_fe_cs); return TRUE; }


void automatch_request(const AutoMatch *m)
{
    InitOnceExecuteOnce(&g_fe_once, fe_init, NULL, NULL);
    EnterCriticalSection(&g_fe_cs);
    g_req = *m;
    if (g_req.count < 2) g_req.count = 2;
    if (g_req.count > 4) g_req.count = 4;
    InterlockedExchange(&g_armed, 1);
    LeaveCriticalSection(&g_fe_cs);
    fprintf(stderr, "[AUTOMATCH] armed: type %d, venue %d, %d fighters\n", g_req.type, g_req.venue, g_req.count);
}

int automatch_pending(void) { return g_armed || g_running; }

/* "type=0,venue=1,f0=55,p0=0,f1=22,p1=-1" (pads: -1 is the CPU) -> a request. */
int automatch_request_text(const char *e)
{
    AutoMatch m;
    char key[8];
    int value, n = 0;
    if (!e || !*e) return 0;
    memset(&m, 0, sizeof(m));
    m.venue = 1;
    m.fighter[0] = 55; m.fighter[1] = 22; m.pad[0] = 0; m.pad[1] = -1;
    while (*e) {
        if (sscanf(e, "%7[a-z0-9]=%d%n", key, &value, &n) == 2) {
            if (!strcmp(key, "type")) m.type = value;
            else if (!strcmp(key, "rules")) m.rules = value;
            else if (!strcmp(key, "venue")) m.venue = value;
            else if (key[0] == 'f' && key[1] >= '0' && key[1] <= '3') m.fighter[key[1] - '0'] = value;
            else if (key[0] == 'p' && key[1] >= '0' && key[1] <= '3') m.pad[key[1] - '0'] = value;
            if (key[0] == 'f' && key[1] >= '0' && key[1] <= '3' && key[1] - '0' + 1 > m.count) m.count = key[1] - '0' + 1;
        }
        e += n ? n : 1;
        n = 0;
        while (*e == ',' || *e == ' ') e++;
    }
    if (m.count < 2) m.count = 2;
    automatch_request(&m);
    return 1;
}

static void read_env(void)
{
    g_env_read = 1;
    automatch_request_text(getenv("RECOMP_AUTOMATCH"));
}

/* Tests (src/hooks/test_control.c): front-end commands to run on the game thread at the next call. */

int automatch_queue_fe(const char *command)
{
    int ok = 0;
    InitOnceExecuteOnce(&g_fe_once, fe_init, NULL, NULL);
    EnterCriticalSection(&g_fe_cs);
    if (g_fe_count < 4) { snprintf(g_fe_queue[g_fe_count], sizeof(g_fe_queue[0]), "%s", command); g_fe_count++; ok = 1; }
    LeaveCriticalSection(&g_fe_cs);
    return ok;
}

static void run_fe_queue(void)
{
    char cmds[4][160];
    int n, i;
    if (!g_fe_count) return;
    EnterCriticalSection(&g_fe_cs);
    n = g_fe_count;
    memcpy(cmds, g_fe_queue, sizeof(cmds));
    g_fe_count = 0;
    LeaveCriticalSection(&g_fe_cs);
    for (i = 0; i < n; i++) {
        unsigned int r = defjam_fe_call(cmds[i]);
        fprintf(stderr, "[CONTROL] %s -> 0x%08X\n", cmds[i], r);
    }
}

static void call(const char *format, ...)
{
    if(g_running && !netplay_active())return;
    char command[160];
    va_list args;
    unsigned int result;
    va_start(args, format);
    vsnprintf(command, sizeof(command), format, args);
    va_end(args);
    xbox_NoteAnchorEvent(command);     /* a pad script can wait for these as for the front end's own */
    result = defjam_fe_call(command);
    fprintf(stderr, "[AUTOMATCH] %s -> %s\n", command, result ? "ok" : "0");
}

/* Online matches are fought on Hard, the same on every PC: the reported difference (players lost
 * health much faster online) came from each PC's own Options setting, which also changes the
 * fight, so different settings would part the lockstep. The player's own setting comes back on the
 * main menu once the online session is over. */
#define ONLINE_DIFFICULTY 2
static int g_saved_difficulty = -1;

/* A front-end call's answer as a number: a small value as it is, or the text it points to
 * ("2", or a name); -1 if it says nothing usable. */
static int fe_number(const char *command)
{
    unsigned int p = defjam_fe_call(command), k;
    char text[16];
    if (p < 16) return (int)p;
    for (k = 0; k < sizeof(text) - 1; k++) {
        char c = (char)MEM8(p + k);
        if (!c) break;
        text[k] = (char)tolower((unsigned char)c);
    }
    text[k] = 0;
    if (text[0] >= '0' && text[0] <= '9') return atoi(text);
    if (strstr(text, "easy")) return 0;
    if (strstr(text, "med") || strstr(text, "norm")) return 1;
    if (strstr(text, "hard")) return 2;
    return -1;
}

static void difficulty_for_online(void)
{
    /* RECOMP_ONLINE_DIFFICULTY=keep or 0..2: tests compare the settings */
    const char *e = getenv("RECOMP_ONLINE_DIFFICULTY");
    int want = e && *e >= '0' && *e <= '2' ? *e - '0' : ONLINE_DIFFICULTY;
    if (e && !strcmp(e, "keep")) return;
    if (g_saved_difficulty < 0) {
        g_saved_difficulty = fe_number("Option.GetDifficulty()");
        fprintf(stderr, "[AUTOMATCH] the player's difficulty is %d; online matches use %d\n",
                g_saved_difficulty, want);
    }
    call("Option.SetDifficulty(%d)", want);
}

static void difficulty_back(void)
{
    if (g_saved_difficulty < 0) return;
    call("Option.SetDifficulty(%d)", g_saved_difficulty);
    g_saved_difficulty = -1;
}

static void run_sequence(void)
{
    if(!netplay_active()){InterlockedExchange(&g_armed,0);return;}
    AutoMatch copy;
    EnterCriticalSection(&g_fe_cs); copy = g_req; LeaveCriticalSection(&g_fe_cs);
    const AutoMatch *m = &copy;
    int i, humans = 0;
    g_running = 1;
    InterlockedExchange(&g_armed, 0);
    for (i = 0; i < m->count; i++) if (m->pad[i] >= 0) humans++;
    netplay_sync_point();
    if(!netplay_active()){g_running=0;return;}
    if(!android_netplay_prepare_ports(humans)){g_running=0;return;}
    if (netplay_match_abandoned()) {     /* the other player left while this PC got here */
        g_running = 0;
        return;
    }
    difficulty_for_online();
    call("Game.StartBattleMode()");
    call("Game.ResetMatchTypeList()");
    call("Game.ResetMatchData()");
    call("Game.SetMatchType(%d)", m->type);
    call("Game.SetMatchRules(%d)", m->rules);
    call("Controller.ControllerBind(-1)");
    for (i = 0; i < m->count; i++) if (m->pad[i] >= 0) call("Controller.ControllerBind(%d)", m->pad[i]);
    for (i = 0; i < m->count; i++) if (m->pad[i] >= 0) call("MemCard.SetBMCurrentUserIndex(%d,-1)", m->pad[i]);
    call("Battle.LoadCreatePlayerData(%d)", humans);
    call("Controller.ControllerSetup(-1,0,0,0)");
    call("Game.ResetFighterList()");
    for (i = 0; i < m->count; i++) call("Controller.ControllerSetup(%d,%d,%d,%d)", i, m->pad[i], m->fighter[i], i);
    call("Game.ResetVenueList(0)");
    call("Game.ChooseVenue(%d)", m->venue);
    call("Controller.GetCustomControlMap()");
    call("Game.GetMatchInfo()");
    call("Game.StartGame()");
    g_running = 0;
    fprintf(stderr, "[AUTOMATCH] sequence done\n");
    fflush(stderr);
}

/* RECOMP_FE_PROBE=1: once the main menu has settled, list for every match type what the venue
 * screen would offer (Game.ResetVenueList / GetNextVenue), and the match types and the roster
 * once, as [FE-PROBE] lines. For working out which venues each mode allows; a probe run only. */
static void probe_list(const char *reset, const char *next, const char *tag, int limit)
{
    char first[200] = "";
    int i;
    defjam_fe_call(reset);
    for (i = 0; i < limit; i++) {
        unsigned int p = defjam_fe_call(next), k;
        char out[200];
        for (k = 0; p && k < sizeof(out) - 1; k++) {
            char c = (char)MEM8(p + k);
            if (!c) break;
            out[k] = c;
        }
        out[p ? k : 0] = 0;
        if (!out[0] || !strcmp(out, first)) break;
        if (!i) strcpy(first, out);
        fprintf(stderr, "[FE-PROBE] %s %s\n", tag, out);
    }
}

static void run_probe(void)
{
    int t;
    char cmd[64], tag[32];
    g_running = 1;
    defjam_fe_call("Game.StartBattleMode()");
    probe_list("Game.ResetMatchTypeList()", "Game.GetNextMatchType()", "matchtype", 20);
    probe_list("Game.ResetFighterList()", "Game.GetNextFighter()", "fighter", 200);
    for (t = 0; t <= 12; t++) {
        snprintf(cmd, sizeof(cmd), "Game.SetMatchType(%d)", t);
        defjam_fe_call(cmd);
        snprintf(tag, sizeof(tag), "venue[type %d]", t);
        probe_list("Game.ResetVenueList(0)", "Game.GetNextVenue()", tag, 60);
    }
    defjam_fe_call("Game.SetMatchType(0)");
    g_running = 0;
    fprintf(stderr, "[FE-PROBE] done\n");
    fflush(stderr);
}

static void print_result(const char *text)
{
    unsigned int p = g_eax, i;
    char out[400];
    if (g_results < 0) g_results = getenv("RECOMP_FE_RESULTS") != NULL;
    if (!g_results || !p) return;
    if (strncmp(text, "Game.GetNext", 12) && strncmp(text, "Game.GetMatchInfo", 17) && strncmp(text, "Game.GetMatchType", 17))
        return;
    for (i = 0; i < sizeof(out) - 1; i++) {
        char c = (char)MEM8(p + i);
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
    fprintf(stderr, "[FE-RESULT] %s = %s\n", text, out);
}

void test_control_note_funccall(const char *text);     /* src/hooks/test_control.c */

void automatch_on_funccall(const char *text)
{

    run_fe_queue();
    unsigned int keep = g_eax;
    if (!g_env_read) read_env();

    android_netplay_menu_event(text);
    extern void online_menu_on_funccall(const char *);
    online_menu_on_funccall(text);
    print_result(text);
    if (g_running) return;
    /* the main menu, or the battle match-type screen the summary of a match returns to */
    if (strstr(text, "intMain/mainMenu.swf") || strstr(text, "GetScreenInfo(battle/cmtype")) {
        g_on_menu = 1;
        g_quiet = g_ticks = 0;
        if (!g_armed) netplay_on_main_menu();       /* an online match has ended */
        if (!g_armed && strstr(text, "intMain/mainMenu.swf") && !netplay_in_lobby())
            difficulty_back();                      /* the online session is over */
    }
    else if (strstr(text, "GetScreenInfo(")) g_on_menu = 0;
    if (!strncmp(text, "MemCard.", 8)) g_quiet = g_ticks = 0;
    else if (!strcmp(text, "Online.Tick()")) { if (g_on_menu) g_ticks++; }
    else if (g_on_menu) g_quiet++;
    {
        static int probed;
        if (!probed && g_on_menu && (g_quiet >= SETTLE_CALLS || g_ticks >= SETTLE_TICKS || !strcmp(text, "Audio.PlayMusic()")) &&
            getenv("RECOMP_FE_PROBE")) {
            probed = 1;
            run_probe();
        }
    }
    if (g_armed && g_on_menu && (g_quiet >= SETTLE_CALLS || g_ticks >= SETTLE_TICKS || !strcmp(text, "Audio.PlayMusic()"))) {
        g_on_menu = 0;
        run_sequence();
    }
    g_eax = keep;           /* the front end gets its own call's answer back */
}


/* The Android game data stays untouched. Tick the existing screen-manager hook
 * instead of patching the disc's Flash menu to emit Online.Tick(). */
void automatch_android_tick(void) {
    if (__atomic_load_n(&g_armed,__ATOMIC_ACQUIRE) && !g_running && g_on_menu) automatch_on_funccall("Online.Tick()");
    else if(g_saved_difficulty>=0 && g_on_menu && !netplay_in_lobby() && !netplay_active())difficulty_back();
}
int automatch_android_main_menu(void) { return g_on_menu && !g_running; }
void automatch_android_cancel(void){InterlockedExchange(&g_armed,0);}

#endif
