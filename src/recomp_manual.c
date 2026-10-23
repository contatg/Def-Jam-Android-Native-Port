/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "host.h"
#if defined(__ANDROID__)
#include "../tools/xboxrecomp/src/kernel/android_gpu_wait.h"
#endif

/* The generated register model: MEM8/16/32, XBOX_PTR, and the guest registers
 * declared RECOMP_TLS. Declaring them here by hand instead would resolve to
 * the image's TLS template rather than the running thread's copy, so an
 * override would read zeroes for every register. */
#include "recomp/gen/recomp_types.h"
#include "hooks/save_compat.h"
#include "hooks/test_observations.h"
#include "hooks/recorder.h"
#include "hooks/fe_inject.h"
#include "hooks/scene_launcher.h"
#include "hooks/dev_module.h"
#include "patches/picture.h"

/* Crowd test controls (src/hooks/crowd_control.c, RECOMP_TEST_CROWD; off unless set). */
void defjam_crowd_match_start(void);
void defjam_crowd_update(void);
void defjam_crowd_seed(uint32_t generator, uint32_t stack);
void defjam_crowd_step(void);

/* Read-only probes use the lifter's universal entry hooks. They do not alter
 * guest registers, stack, memory or the generated body. Off in normal play. */
void sub_001A4A30_enter(void) { defjam_test_update(); }
void sub_001A5DD0_enter(void)
{
    if (defjam_test_observations_enabled())
        defjam_test_hit(MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp));
}
void sub_001A50B0_enter(void)
{
    if (defjam_test_observations_enabled())
        defjam_test_health_begin(MEM32(g_esp + 4), MEM32(g_esp));
}
void sub_001A6870_enter(void)
{
    if (defjam_test_observations_enabled())
        defjam_test_health_notify(MEM32(g_esp + 4), MEM32(g_esp + 8));
}
/* Match telemetry (src/hooks/test_telemetry.c): the fight's simulation step,
 * match set-up, the result recorder and the random number generator's seeding.
 * Read-only, except that RECOMP_TEST_RNG_SEED replaces the seed argument. */
void sub_00087700_enter(void)
{
    extern void nv2a_ws_hud_heartbeat(int bottom_rows);
    uint32_t i, fighters = 0;
    defjam_crowd_step();
    defjam_test_step();
    /* Widescreen: a fight HUD is up (src/patches/widescreen.c). The four fighter
     * slots the fight camera reads (0x340D70, stride 0xC8; a slot counts when set
     * and bit 3 of +0xC0 is clear): with 3 or 4 fighters the HUD has bottom rows. */
    for (i = 0; i < 4; i++)
        if (MEM32(0x340D70 + i * 0xC8) != 0 && (MEM8(0x340D70 + i * 0xC8 + 0xC0) & 8) == 0)
            fighters++;
    nv2a_ws_hud_heartbeat(fighters >= 3);
}
void sub_001A4D90_enter(void) { defjam_test_match_start(); defjam_crowd_match_start(); }
void sub_001BC690_enter(void) { defjam_test_rng(g_esp); }
void sub_001AD220_enter(void) { defjam_test_ai_seed(g_esp); }
/* The seeding routine every generator of this kind goes through (thiscall). */
void sub_000AA0D0_enter(void) { defjam_test_generator_seed(g_ecx, g_esp); defjam_crowd_seed(g_ecx, g_esp); }
/* Every draw (RECOMP_TEST_DRAWS): the C runtime's rand, the match generator, and the two
 * wrappers the object-form generators are drawn through; then the background crowd's update. */
void sub_00200820_enter(void) { defjam_test_draw(1, MEM32(g_esp)); }
void sub_001BC6E0_enter(void) { defjam_test_draw(2, MEM32(g_esp)); }
void sub_000AA300_enter(void) { defjam_test_draw(g_ecx, MEM32(g_esp)); }
void sub_000AA340_enter(void) { defjam_test_draw(g_ecx, MEM32(g_esp)); }
void sub_000DB870_enter(void) { defjam_test_crowd_update(); defjam_crowd_update(); }
/* Story scenes (src/hooks/scene_launcher.c): the scene loader names every scene it loads, and
 * the mode system's update plays one on request (RECOMP_SCENE, the overlay) through the game's
 * own developer cinema mode. */
void sub_00073FD0_enter(void) { scene_launcher_loader_enter(); }
void sub_0002DFF0_enter(void) { scene_launcher_mode_enter(); }
/* The developer run module, rebuilt from the host (src/hooks/dev_module.c): the manager's tick
 * queues it on request; its methods are traced. RECOMP_DEV_MODULE; off unless set. */
void sub_00077040_enter(void) { dev_module_manager_enter(); }
void sub_00077C40_enter(void) { dev_module_trace(1); }
void sub_00077A40_enter(void) { dev_module_trace(3); }
void sub_00077B20_enter(void) { dev_module_trace(4); }
void sub_00077DD0_enter(void) { dev_module_trace(7); }
/* The per-fighter pad reader: RECOMP_TEST_INPUT replaces what it is about to read. */
void sub_001BAA00_enter(void) { defjam_test_input(MEM32(g_esp + 4)); }
void sub_001A6A80_enter(void)
{
    if (defjam_test_observations_enabled())
        defjam_test_result(MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12));
}

/* â”€â”€ ICALL trace ring buffer â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* Upstream's lifter reports reached instructions it cannot translate. Keep
 * this visible in automated validation, with its optional first-hit trap. */
void recomp_unimpl(const char *text, uint32_t va)
{
    static volatile LONG printed;
    static volatile LONG known_printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';
    /* These instructions were already omitted in the accepted baseline
     * (TODO lines in its generated C). Keep them visible and separate from new
     * omissions; repeated IRQ/cache instructions must not exhaust that budget. */
    int known = !strcmp(text, "cli") || !strcmp(text, "sti") || !strcmp(text, "wbinvd");
    /* Exact PUSHFD/POPFD sites from the preserved baseline's lift summary and
     * generated TODOs. Other sites remain failures. Implementing their flags
     * and interrupt effects is a separate runtime project, not this migration. */
    if (!strcmp(text, "pushfd"))
        known = va == 0x00088500u || va == 0x0009F2C0u || va == 0x00130B46u
             || va == 0x002470B8u || va == 0x002470C4u;
    else if (!strcmp(text, "popfd"))
        known = va == 0x000330A4u || va == 0x000C3FE4u || va == 0x00130B50u
             || va == 0x002470C3u;
    LONG n = InterlockedIncrement(known ? &known_printed : &printed);
    if (n <= (known ? 10 : 50) || stop) {
        fprintf(stderr, "%s untranslated instruction REACHED: `%s` at 0x%08X\n",
                known ? "[UNIMPL-KNOWN]" : "[UNIMPL]", text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/* Keep the corrected name hash, with a read-only fallback for saves created
 * before upstream fixed SHRD at count zero. A complete replacement ensures
 * direct and indirect callers both use the compatibility check. */
void sub_001F8360(void)
{
    uint32_t name = MEM32(g_esp + 4);
    uint32_t destination = MEM32(g_esp + 8);
    uint32_t capacity = MEM32(g_esp + 12);
    char folder[13];
    int legacy = defjam_save_directory_name((const uint16_t *)XBOX_PTR(name), folder);
    if (capacity >= sizeof(folder)) {
        memcpy((void *)XBOX_PTR(destination), folder, sizeof(folder));
        g_eax = (unsigned char)folder[0];
    } else {
        g_eax = 0;
    }
    if (legacy) {
        static volatile LONG printed;
        if (InterlockedIncrement(&printed) <= 8)
            fprintf(stderr, "[SAVE-COMPAT] using verified existing folder %s\n", folder);
    }
    g_esp += 16; /* stdcall: return address and three arguments */
}

/* â”€â”€ Manual overrides for this title â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * memmove/memcpy, XDK 5849 CRT, guest 0x002016B0 (19 direct callers).
 *
 * The lifted version is wrong. MSVC's copy routine dispatches its
 * alignment and tail cases through jump tables embedded in the body
 * (`jmp dword ptr [edx*4 + 0x2017FC]` and friends). Function detection
 * ends the body at 0x00201952, but those tables' arms live at
 * 0x0020194C-0x002019D4, past that end -- so the lifter could not prove
 * they were intra-function, emitted an indirect call instead of a goto,
 * and at runtime the dispatch found no such function:
 *
 *   [ICALL] Failed to resolve VA 0x00201770 (total calls: 2485)
 *   [ICALL] Failed to resolve VA 0x00201954
 *
 * `rep movsd` still copied the aligned bulk, so most calls looked fine
 * and only the head/tail bytes were dropped. A pointer copied by one of
 * those calls came out half-written, which is where the null indirect
 * calls that followed came from.
 *
 * Reimplementing it natively is both the fix and a large speed-up; the
 * original is cdecl, returns the destination in eax, and handles
 * overlap, which is C memmove exactly. Callers must already treat
 * eax/ecx/edx as clobbered, so leaving ecx/edx alone is safe.
 */
void sub_002016B0(void)
{
    uint32_t dst = MEM32(g_esp + 4);
    uint32_t src = MEM32(g_esp + 8);
    uint32_t n   = MEM32(g_esp + 12);

    if (n)
        memmove((void *)XBOX_PTR(dst), (const void *)XBOX_PTR(src), (size_t)n);

    g_eax = dst;
    g_esp += 4;          /* cdecl: consume the return address, caller pops args */
}

/* DirectSound's idle-voice lookup, XDK DSOUND section, guest 0x0025FB7C.
 *
 * When the audio chip traps to say a hardware voice has gone idle, the
 * interrupt routine calls this with the voice's id (thiscall: ecx = the voice
 * manager, one stack argument, `ret 4`). It looks the voice's client up in the
 * manager's 256-entry table at +0x88 and, if the client still owns that voice
 * and is on a list, has sub_002626B5 take it off the hardware voice list.
 *
 * The original does not test the table entry for null: on a console a trapped
 * voice always has a client, because the trap is serviced within microseconds.
 * Here the chip model's frame thread raises the trap and the routine runs about
 * a millisecond later, and the model lets a later method overwrite the trapped
 * voice id, so the entry can be empty by then. With a null client and voice id
 * 0 every test below passes on zero-page reads and sub_002626B5 is called with
 * this == NULL: the read fault at 0xFFFFFFBE seen once in three Debug runs of
 * the four-fighter Terrordome match (docs/research/dsound-voice-list-crash.md).
 *
 * This is the original's logic with that one test added. An empty entry means
 * there is no client to take off any list, so returning is what the original
 * does for every other "nothing to do" case. The chip model's trap handling is
 * the real fault and is still to be fixed; the count below says how often the
 * guard is what saved a run. */
extern void sub_002626B5(void);
void sub_0025FB7C(void)
{
    uint32_t voice = MEM32(g_esp + 4);
    uint32_t manager = g_ecx;

    g_eax = voice;
    if (voice < 0x100u && MEM32(manager + 0x84) == 0) {
        uint32_t voices = MEM32(0x27CCB0);
        g_edx = voice << 7;
        if (!(MEM32(g_edx + voices + 4) & 0x800000u)) {
            uint32_t client = MEM32(manager + voice * 4 + 0x88);
            g_ecx = client;
            if (!client) {
                static volatile LONG empty;
                LONG n = InterlockedIncrement(&empty);
                if (n <= 8 || (n & (n - 1)) == 0) {
                    fprintf(stderr, "[DSOUND] idle trap for voice %u with no client; skipped (%ld so far)\n",
                            voice, (long)n);
                    fflush(stderr);
                }
            } else {
                g_edx = MEM8(client + 0x64);
                g_edx = MEM16(client + g_edx * 2 + 0xA);
                if (voice == g_edx) {
                    g_eax = client + 0x4C;
                    if (MEM32(g_eax) != g_eax) {
                        PUSH32(g_esp, 1);
                        PUSH32(g_esp, 0x0025FBC9u);
                        sub_002626B5();
                    }
                }
            }
        }
    }
    g_esp += 8;          /* ret 4: the return address and the argument */
}

/* DirectSound's GP DSP mailbox, XDK DSOUND section, guest 0x0025F530.
 *
 * DirectSound talks to the APU's DSP through a command block in shared
 * memory, [[[this+8]+0x10]]+0x800: it spins until the status word at +0x10
 * is 0 (the DSP took the last command), writes the next one and sets the
 * status to 2. Nothing here runs a DSP, so the first command posted stays
 * at 2 and the second spins for ever. Def Jam posts on tearing a fight's
 * audio down: "No" to a rematch called Game.Quit() and the main thread hung
 * in this loop with the status word (0x831F8810 in that run) at 2
 * (PROGRESS Â§6 2026-09-28).
 *
 * The same writes, minus the wait, with the command marked taken: with no
 * DSP there is nothing it would have done. thiscall, two stack arguments:
 * a length or offset (used when the second is non-zero) and a count; with
 * the second 0 the pending pair in [this+0x18]/[this+0x1C] is posted,
 * unless there is none (0x8000 / 0).
 *
 * sub_0025F394 posts command 3 to the same block and waits the same way; it
 * has not been seen to run, so it is left alone. */
/* Direct3D's timer callback (0x00216410, D3DDevice_SetTimerCallback's body:
 * time in TSC ticks, callback, context), with one change: a time already
 * past is armed at once instead of refused.
 *
 * Def Jam's frame clock is a chain of these: the callback (0x000D7EA0, run in
 * Direct3D's PTIMER DPC) asks for the next frame at now + period - lateness
 * (0x000D7DE0). Delivered more than a frame late, that time has passed, the
 * original returns 0x8876082A without arming anything, nothing retries, and
 * the title's tick never runs again -- the screen keeps drawing while its
 * timeline stands still. On the console the DPC is never that late; here a
 * host stall during start-up's loading made it so in about one start in four
 * (the legal screen), once the PTIMER count stopped running away (0074).
 * Armed at once, the callback runs, the title's own lateness correction
 * catches up, and the chain continues. Otherwise identical: raise to
 * DISPATCH_LEVEL, hand the delay to 0x00224082 on the device's timer object,
 * lower, return S_OK. */
extern void sub_00224082(void);
extern uint64_t xbox_ReadTimeStampCounter(void);
extern unsigned char __stdcall xbox_KeRaiseIrqlToDpcLevel(void);
extern void __fastcall xbox_KfLowerIrql(unsigned char NewIrql);

void sub_00216410(void)
{
    uint32_t t_lo = MEM32(g_esp + 4), t_hi = MEM32(g_esp + 8);
    uint32_t cb = MEM32(g_esp + 12), ctx = MEM32(g_esp + 16);
    uint64_t delta = 0;
    unsigned char old = xbox_KeRaiseIrqlToDpcLevel();

    if (cb) {
        uint64_t target = ((uint64_t)t_hi << 32) | t_lo;
        uint64_t now = xbox_ReadTimeStampCounter();
        if (target <= now + 0x28) {
            static unsigned late;
            if (late++ < 8 || (late & (late - 1)) == 0)
                fprintf(stderr, "[TIMER] callback 0x%08X asked for a time %llu us past;"
                                " armed at once (#%u)\n", cb,
                        (unsigned long long)((now - target) / 733), late);
            target = now + 0x28;
        }
        delta = target - now;
    }
    PUSH32(g_esp, ctx);
    PUSH32(g_esp, cb);
    PUSH32(g_esp, (uint32_t)(delta >> 32));
    PUSH32(g_esp, (uint32_t)delta);
    g_ecx = MEM32(0x00234768u) + 0x1C28u;
    PUSH32(g_esp, 0x00216478u);
    sub_00224082();                 /* thiscall, pops its return and 0x10 */
    xbox_KfLowerIrql(old);
    g_eax = 0;
    g_esp += 4 + 0x10;              /* ret 0x10 */
}

void sub_0025F530(void)
{
    uint32_t self = g_ecx;
    uint32_t a1 = MEM32(g_esp + 4), a2 = MEM32(g_esp + 8);
    uint32_t len, count, box;

    if (a2 == 0) {
        len = MEM32(self + 0x18);
        count = MEM32(self + 0x1C);
        if (len == 0x8000 || count == 0)
            goto done;
    } else {
        len = a1;
        count = a2;
    }
    box = MEM32(MEM32(MEM32(self + 8) + 0x10)) + 0x800;
    MEM32(box) = (len >> 2) - MEM32(box + 4) - 0x206;
    MEM32(box + 8) = len;
    MEM32(box + 0xC) = count >> 2;
    MEM32(box + 0x10) = 0;          /* 2 on hardware: posted, until the DSP takes it */
    MEM32(self + 0x1C) = 0;
    MEM32(self + 0x18) = 0x8000;
    g_eax = box;
    g_edx = count >> 2;
done:
    g_esp += 12;         /* thiscall, ret 8 */
}

/* â”€â”€ Manual function overrides â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */


/* ---- A wrapper that does not wrap -------------------------------------
 *
 * Two attempts lived here: `_gen` wrappers on 0x00197650 and 0x0019B0C0, to
 * find out whether the front-end constructor ran and what its allocation
 * returned. Neither ever printed anything, and the reason is worth keeping.
 *
 * Declaring `extern void sub_X_gen(void);` does emit the real body under that
 * name -- and it also rewrites every direct call site to call `sub_X_gen`
 * instead. So a direct caller goes straight to the original body and the
 * wrapper is never on the path. It only intercepts calls that arrive through
 * recomp_lookup_manual(), which sees indirect calls only. The sleep trace
 * below works for exactly that reason: its target is reached through a
 * function pointer.
 *
 * To trace a function with direct callers, either replace it outright (define
 * it here without the `_gen` declaration and implement the behaviour), or put
 * a hardware watchpoint on something it writes -- src/hooks/watchpoint.c, and
 * scripts/guest-stack.py to read the call chain once it fires.
 */

/* ---- Who is sleeping? ------------------------------------------------
 *
 * The title reaches its loading screen and stays there, and the indirect-call
 * ring (scripts/icall-window.py) says the busiest thing it dispatches is
 * guest 0x000D1270, a two-instruction shim onto XAPI's Sleep. So game code is
 * running and it is waiting for something in a poll-and-sleep loop.
 *
 * Which code is the question this answers. The shim is reached through a
 * function pointer, so recomp_lookup_manual() can intercept it without the
 * lifter being involved at all: on entry the guest stack top is the caller's
 * return address, which names the call site exactly. Then the real body runs,
 * so behaviour is unchanged.
 *
 * Set RECOMP_TRACE_SLEEP to turn it on. Off by default: the interception costs
 * nothing, but the histogram is only meaningful while this question is open.
 */
extern void sub_000D1270(void);

static void trace_sleep_caller(void)
{
    enum { SLOTS = 24 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    uint32_t ret = MEM32(g_esp);
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == ret)
            break;
    if (i == count) {
        if (count < SLOTS) {
            seen[count] = ret;
            hits[count] = 0;
            count++;
        } else {
            sub_000D1270();
            return;
        }
    }
    hits[i]++;
    /* Report at 1, 10, 100, ... so a rare caller shows up once and a loop
     * shows up as a progression, without a line per sleep. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n == 1)
            fprintf(stderr, "[SLEEP] called from 0x%08X -- %llu time(s)\n",
                    ret, (unsigned long long)hits[i]);
        fflush(stderr);
    }
    sub_000D1270();
}

/* ---- The front end's calls into the game --------------------------------
 *
 * Each screen's ActionScript reaches the game through FuncCall, which hands a
 * string such as "Controller.ControllerBind(0)" or
 * "Control.GetScreenInfo(intMain/mainMenu.swf)" to GetURL, and the game's
 * handler, guest 0x00068BA0, dispatches it by module and method
 * (sub_000457D0). That handler is reached through a function pointer only, so
 * it can be wrapped here. Each call is printed as [FUNCCALL] -- the front
 * end's own account of what it is doing, a few a second at most -- and
 * offered to the script anchors, so a pad script can wait for a screen:
 * "@getscreeninfo(intmain/mainmenu" (matched without case).
 */
extern void defjam_skip_intro_enter(void);
void sub_0006BC90_enter(void) { defjam_skip_intro_enter(); }

extern void sub_00068BA0(void);
extern void xbox_NoteAnchorEvent(const char *text);
extern void defjam_unlock_all_on_funccall(const char *text);

static void funccall_hook(void)
{
    static long printed;
    char text[192];
    uint32_t p = MEM32(g_esp + 4);
    int i;

    for (i = 0; p && i < (int)sizeof text - 1; i++) {
        char c = (char)MEM8(p + (uint32_t)i);
        if (!c)
            break;
        text[i] = c;
    }
    text[i] = 0;
    if (printed++ < 4000)
        fprintf(stderr, "[FUNCCALL] %s\n", text);
    /* The play-test recorder keeps every one, with its time (the log stops at 4000). */
    recorder_event_text("fe", "text", text);
    xbox_NoteAnchorEvent(text);
    defjam_unlock_all_on_funccall(text);   /* RECOMP_UNLOCK_ALL, src/patches/dev_unlock.c */
    sub_00068BA0();
    /* RECOMP_FE_INJECT: commands of a test's own, after a chosen call (src/hooks/fe_inject.c). */
    fe_inject_after_call(text);
    dev_module_after_call(text);
}

/* ---- Trace chosen functions, by guest address --------------------------
 *
 * The generalisation of the sleep trace below. Set

 *     RECOMP_TRACE_ICALL=0x000D7060,0x0019AB60
 *
 * and every call to one of those addresses is reported -- the first one, then
 * at ten, a hundred, a thousand, so a function called once and a function
 * called in a loop read differently without a line per call. The caller is
 * reported too: on entry the guest stack top is the return address.
 *
 * This only sees calls that arrive indirectly, through a vtable or a function
 * pointer, because that is all recomp_lookup_manual() is asked about. That
 * covers more than it sounds like: a title's per-frame work and all of its
 * state machines go through vtables, and a direct call compiles to an ordinary
 * C call that nothing can intercept without re-lifting.
 *
 * Answering "does this ever run at all" is what it is for. That question came
 * up for the intro sequence at 0x000D7060, which plays movies/eagames,
 * movies/thx and movies/intro from a six-state switch -- and none of those
 * files is ever opened.
 */

#define TRACE_SLOTS 8

static uint32_t s_trace_va[TRACE_SLOTS];
static uint64_t s_trace_hits[TRACE_SLOTS];
static int      s_trace_count = -1;

static void trace_parse(void)
{
    const char *list = getenv("RECOMP_TRACE_ICALL");
    s_trace_count = 0;
    if (!list || !*list)
        return;
    while (*list && s_trace_count < TRACE_SLOTS) {
        char *end;
        unsigned long v = strtoul(list, &end, 0);
        if (end == list)
            break;
        s_trace_va[s_trace_count++] = (uint32_t)v;
        list = (*end == ',' || *end == ' ') ? end + 1 : end;
    }
    fprintf(stderr, "[TRACE] watching %d indirect target(s)\n", s_trace_count);
    fflush(stderr);
}

static void trace_report(int slot)
{
    uint64_t n = ++s_trace_hits[slot];
    uint64_t m = n;

    while (m >= 10 && m % 10 == 0)
        m /= 10;
    if (m != 1)
        return;
    fprintf(stderr, "[TRACE] 0x%08X called from 0x%08X -- %llu time(s)\n",
            s_trace_va[slot], MEM32(g_esp), (unsigned long long)n);
    fflush(stderr);
}

/* One trampoline per slot, because a C function pointer carries no state and
 * the dispatcher hands the target nothing to identify itself with. Eight is
 * more than any one question has needed. */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

/* recomp_lookup() is the generated table and does not consult this file, so
 * calling it here dispatches the real body rather than recursing. */
static void trace_forward(int slot)
{
    recomp_func_t fn = recomp_lookup(s_trace_va[slot]);
    if (fn)
        fn();
}

static void trace_slot_0(void) { trace_report(0); trace_forward(0); }
static void trace_slot_1(void) { trace_report(1); trace_forward(1); }
static void trace_slot_2(void) { trace_report(2); trace_forward(2); }
static void trace_slot_3(void) { trace_report(3); trace_forward(3); }
static void trace_slot_4(void) { trace_report(4); trace_forward(4); }
static void trace_slot_5(void) { trace_report(5); trace_forward(5); }
static void trace_slot_6(void) { trace_report(6); trace_forward(6); }
static void trace_slot_7(void) { trace_report(7); trace_forward(7); }

static const recomp_func_t s_trace_slots[TRACE_SLOTS] = {
    trace_slot_0, trace_slot_1, trace_slot_2, trace_slot_3,
    trace_slot_4, trace_slot_5, trace_slot_6, trace_slot_7,
};

/* RECOMP_TRACE_VOICE=1: EA's software voice render (0x0012C310, reached
 * through the voice object's vtable) -- its history flag, pitch and saved
 * history on entry, what it returned and the first samples it wrote. Written
 * to find why every 512-sample block of the six-channel mix began with four
 * zero samples. */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
static void trace_voice_render(void)
{
    static unsigned shown;
    static uint64_t first;
    uint32_t st = MEM32(g_esp + 4), n = MEM32(g_esp + 8);
    uint32_t dec = MEM32(g_esp + 12), out = MEM32(g_esp + 16);
    uint16_t flag = MEM16(st + 0x28), pad = MEM16(st + 0x2A);
    uint32_t pitch = MEM32(st + 0x1C);
    float h[4];
    uint64_t now = GetTickCount64();
    int show;
    recomp_func_t fn = recomp_lookup(0x0012C310u);
    memcpy(h, (const void *)XBOX_PTR(st + 0x2C), sizeof h);
    if (!first)
        first = now;
    show = now - first > 30000 && shown < 60;
    if (fn)
        fn();
    if (show) {
        float o[6];
        memcpy(o, (const void *)XBOX_PTR(out), sizeof o);
        shown++;
        fprintf(stderr, "[VOICE] st %08X n %u pitch %08X flag %u pad %u hist %g %g %g %g"
                        " -> %u; out %g %g %g %g %g %g (dec %08X out %08X) resampler %08X pos %08X\n",
                st, n, pitch, flag, pad, h[0], h[1], h[2], h[3], g_eax,
                o[0], o[1], o[2], o[3], o[4], o[5], dec, out, MEM32(st + 0x24), MEM32(st + 0x20));
    }
}

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    if (xbox_va == 0x0012C310u && getenv("RECOMP_TRACE_VOICE"))
        return trace_voice_render;
    /* This hook sees indirect calls only, so it suits a trace wrapper or a
     * conditional override. A function with direct callers is replaced
     * instead by defining it in this file: the recompiler reads this file and
     * stops generating any body it finds here. */
    int i;

    if (xbox_va == 0x000D1270u && getenv("RECOMP_TRACE_SLEEP"))
        return trace_sleep_caller;
    if (xbox_va == 0x00068BA0u)
        return funccall_hook;
    if (xbox_va == 0x00046E40u) {   /* Game.StartGame, src/patches/dev_unlock.c */
        extern recomp_func_t defjam_start_game_hook(void);
        recomp_func_t w = defjam_start_game_hook();
        if (w)
            return w;
    }

    if (s_trace_count < 0)
        trace_parse();
    for (i = 0; i < s_trace_count; i++)
        if (s_trace_va[i] == xbox_va)
            return s_trace_slots[i];

    return (recomp_func_t)0;
}

/* â”€â”€ ICALL failure logging â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€ */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
extern ptrdiff_t g_xbox_mem_offset;
#if defined(_MSC_VER)
extern __declspec(thread) uint32_t g_esp;
extern __declspec(thread) uint32_t g_ecx;
#else
extern __thread uint32_t g_esp;
extern __thread uint32_t g_ecx;
#endif

/* Set by RECOMP_ITAIL_AT around a tail jump, zero otherwise. */
#ifndef RECOMP_ITAIL_RUNTIME_OWNED
uint32_t g_itail_site;
#endif

void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    if (g_itail_site) {
        /* A tail jump: no return address was pushed, so the stack says
         * nothing. The lifter recorded the jump site instead, and for a
         * switch that is the dispatch whose table was read out of
         * range. */
        fprintf(stderr, "  tail jump at guest 0x%08X (a switch dispatch; "
                        "its jump table read out of range)\n",
                g_itail_site);
        fflush(stderr);
        return;
    }

    /* Where the call was made from, which the target alone never says.
     *
     * The lifter pushes the guest return address immediately before an
     * indirect call, so it is the word at the top of the guest stack when
     * this runs. A target of 0x000000FF is plainly not an address, and the
     * only useful question about it is which call site read a byte where a
     * function pointer belonged. Printing the return address answers that
     * directly: it symbolizes straight back to the caller through the
     * lifted source. */
    if (g_esp) {
        uint32_t ret = *(volatile uint32_t *)((uintptr_t)g_esp
                                            + (uintptr_t)g_xbox_mem_offset);
        fprintf(stderr, "  called from guest 0x%08X (esp 0x%08X)\n", ret, g_esp);
    }

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);

    /* And where from. RECOMP_ICALL_SAFE calls this before restoring the
     * stack, so the guest return address the lifter pushed for the call is
     * still the top word -- which turns "something called a null pointer"
     * into a line of lifted C. */
    if (g_esp) {
        uint32_t ret = *(volatile uint32_t *)((uintptr_t)g_esp
                                            + (uintptr_t)g_xbox_mem_offset);
        fprintf(stderr, "  called from guest 0x%08X\n", ret);
        /* And the object, since most of these are virtual calls: ecx is
         * the this pointer and the first word of it is the vtable. A null
         * slot in a real vtable and a null vtable on a half-built object
         * look identical from the call site and want different fixes. */
        fprintf(stderr, "  this 0x%08X\n", g_ecx);
        /* The stack too. A vtable thunk has no direct callers to grep for,
         * so the only way to learn who invoked it with a null object is to
         * read the frames above it while they are still there. A scan, so
         * some entries are stale -- scripts/guest-stack.py filters the same
         * shape against the real call sites. */
        {
            int k, shown = 0;
            for (k = 0; k < 256 && shown < 16; k++) {
                uint32_t slot = g_esp + (uint32_t)(k * 4);
                uint32_t v;
                if (slot < 0x1000u || slot >= 0x04000000u) break;
                v = *(volatile uint32_t *)((uintptr_t)slot
                                         + (uintptr_t)g_xbox_mem_offset);
                if (v >= 0x00010000u && v < 0x00400000u) {
                    fprintf(stderr, "    [esp+%-3d] 0x%08X\n", k * 4, v);
                    shown++;
                }
            }
        }
        if (g_ecx >= 0x1000u && g_ecx < 0x84000000u) {
            uint32_t vt = *(volatile uint32_t *)((uintptr_t)g_ecx
                                               + (uintptr_t)g_xbox_mem_offset);
            fprintf(stderr, "  this 0x%08X vtable 0x%08X\n", g_ecx, vt);
        }
    }
    fflush(stderr);
}


/*
 * Direct3D's fence insert (sub_0021EB90), entered: hold the calling thread
 * while the push-buffer executor is far behind.
 *
 * Direct3D numbers a fence every time it kicks the push buffer (the counter
 * at device+0x2C, in steps of two) and the GPU reports the last one it
 * reached through the semaphore the device's +0x30 points at. Its free-space
 * arithmetic finds the GPU's position from that fence in a small table, which
 * holds on a console because the GPU is never more than a few fences behind.
 * The optimised build submits faster than the executor draws: at 24 to 40
 * fence numbers of lag Direct3D judged space free that was not, wrote the next
 * lap over commands still to be run ("[PB] D3D overran the executor"), the
 * executor read the new data as commands, and the title died in its own
 * interrupt-time fixup a moment later. A debug build idles at a lag of 2 and
 * peaks around 12.
 *
 * So this waits, yielding, until the lag is back under 16 or 100 ms have
 * passed (the executor may itself be waiting on this thread).
 * RECOMP_FENCE_HOLD=0 turns it off.
 */
void sub_0021EB90_enter(void)
{
    static int on = -1;
    static LARGE_INTEGER freq;
    static unsigned holds, timeouts;
    uint32_t dev, sem;
    LARGE_INTEGER t0, t1;

    if (on < 0) {
        const char *e = getenv("RECOMP_FENCE_HOLD");
        on = !(e && *e == '0');
        QueryPerformanceFrequency(&freq);
    }
    if (!on)
        return;
    dev = MEM32(0x234768);
    if (dev < 0x10000u)
        return;
    sem = MEM32(dev + 0x30);
    if (sem < 0x10000u || (uint32_t)(MEM32(dev + 0x2C) - MEM32(sem)) < 16u)
        return;
    QueryPerformanceCounter(&t0);
    t1 = t0;
    for (;;) {
#if defined(__ANDROID__)
        android_gpu_wait_pause((uint64_t)(t1.QuadPart - t0.QuadPart)
                               * 1000000000ull / (uint64_t)freq.QuadPart);
#else
        SwitchToThread();
#endif
        if ((uint32_t)(MEM32(dev + 0x2C) - MEM32(sem)) < 16u)
            break;
        QueryPerformanceCounter(&t1);
        if ((t1.QuadPart - t0.QuadPart) * 10 > freq.QuadPart) {
            timeouts++;
            break;
        }
    }
    holds++;
    if (holds <= 5 || (holds & (holds - 1)) == 0)
        fprintf(stderr, "[FENCE] held the title for the executor (#%u, %u timed out)\n",
                holds, timeouts);
}

/*
 * The display's gamma ramp.
 *
 * Direct3D's SetGammaRamp ends in sub_002203C6(this, ramp): 256 bytes each of
 * red, green and blue, written one at a time to the RAMDAC's palette port
 * (0xFD6813C9). That page is plain memory here, so the ramp went nowhere. The
 * title sets one at start-up and it is not the identity: an S curve (32 -> 18,
 * 64 -> 52, 128 -> 125, 192 -> 198, 224 -> 233) that deepens the shadows,
 * which is why everything looked lighter than the console. The runtime's
 * Direct3D layer already applies a ramp at presentation (d3d8_gamma.c); this
 * hands it the title's, after the player's brightness (src/patches/picture.c, which
 * also keeps g_recomp_gamma, the same ramp for captures, which are read before
 * presentation). RECOMP_GAMMA=0 leaves the title's ramp out.
 */
void sub_002203C6_enter(void)
{
    uint8_t ramp[3 * 256];
    static int on = -1;
    static unsigned sets;
    uint32_t src = MEM32(g_esp + 4);
    int i;

    if (on < 0) {
        const char *e = getenv("RECOMP_GAMMA");
        on = !(e && *e == '0');
    }
    if (!on || src < 0x10000u)
        return;
    for (i = 0; i < 3 * 256; i++)
        ramp[i] = MEM8(src + i);
    picture_title_ramp(ramp);
    if (sets++ < 4)
        fprintf(stderr, "[GAMMA] ramp set: 32 -> %u, 64 -> %u, 128 -> %u, 192 -> %u, 224 -> %u\n",
                ramp[32], ramp[64], ramp[128], ramp[192], ramp[224]);
}

/*
 * The venue vignette ([display] vignette, vignette_strength; src/patches/picture.c).
 * sub_000D9AF0 is the platform tint's per-frame draw, whose last argument turns the
 * "Vignette Quad" on; sub_00028460 sets a quad's colour, where a weaker vignette
 * gets its alpha and blend.
 */
void sub_000D9AF0_enter(void) { picture_tint_draw_enter(); }
void sub_00028460_enter(void) { picture_quad_color_enter(); }

/*
 * Widescreen (src/patches/widescreen.c, docs/widescreen/02-design.md).
 *
 * sub_0001A4E0 is the scene viewport's GetViewParams(this, mode, &fov, &aspect,
 * &y, &height), thiscall, `ret 0x14`, and the one place every 3D view gets its
 * aspect: a 4:3 literal (mode 0), the cutscenes' 16:9 letterbox (mode 1) or the
 * viewport's own width/height (mode 2). With widescreen off it does exactly what
 * the original did; on, full-screen views are widened (Hor+) and cutscenes follow
 * the cutscene setting.
 */
#include "patches/widescreen.h"

/* Sub-view rectangles follow the centred 2D (src/patches/widescreen.c). */
extern void ws_view_rect_enter(void);
void sub_0001A7A0_enter(void) { ws_view_rect_enter(); }

void sub_0001A4E0(void)
{
    ws_view_params(g_ecx, MEM32(g_esp + 4), MEM32(g_esp + 8), MEM32(g_esp + 12),
                   MEM32(g_esp + 16), MEM32(g_esp + 20));
    g_esp += 24; /* thiscall: return address and five arguments */
}

/* Investigation probes for the 2D placement work (RECOMP_WS_TRACE2D=1; off: nothing). */
extern void ws_trace_pos(void), ws_trace_vtx_arr(void), ws_trace_vtx(void);
void sub_00028F90_enter(void) { ws_trace_pos(); }
void sub_0002E390_enter(void) { ws_trace_vtx_arr(); }
void sub_0002E470_enter(void) { ws_trace_vtx(); }
