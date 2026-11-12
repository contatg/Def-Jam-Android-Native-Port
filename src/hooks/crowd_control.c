/*
 * Test controls for the two crowds: making a pinned fight's pictures repeat.
 *
 * docs/research/crowd-nondeterminism.md found that with every seed pinned, a fight's
 * fighters, HUD and arena repeat pixel for pixel but the crowds do not, because
 * both start animating while the match loads and leave that window in a slightly
 * different state each run:
 *
 *  - the background crowd (sub_000DB870, run when the byte at 0x2E3488 is set) is
 *    updated 902 times in one run's loading window and 903 in another's, and each
 *    update draws from the C runtime's rand, so every later draw is shifted;
 *  - the foreground spectators draw their animation variants from the generator
 *    at 0x375F68 a different number of times in the same window.
 *
 * Two controls, selected with RECOMP_TEST_CROWD (comma-separated), only while the
 * test probes are on (RECOMP_TEST_OBSERVATIONS=1):
 *
 *   hold[:N]   let the background crowd update N times after match set-up (default
 *              880, below every count measured), then clear 0x2E3488 so it holds
 *              still until the first fight step, where the byte is put back. Every
 *              run then enters the fight with the crowd updated exactly N times.
 *              (Clearing the byte for the whole window crashed at the first step,
 *              because the first updates also set up the crowd's drawing; N
 *              updates do that first.)
 *   reseed     at the first fight step, re-seed every object-form generator that
 *              was seeded during set-up (0x375F68, 0x376FDC, ...), through the
 *              game's own seeding routine sub_000AA0D0, from RECOMP_TEST_RNG_SEED.
 *              Whatever the window drew, the spectators' draws from then on are
 *              the same in every run. It changes the fight's state stream (one of
 *              these generators feeds the fight), so a stream recorded with it on
 *              is only comparable with runs that also have it on.
 *
 * Neither is proven yet on the game: scripts/crowd-lab.py runs the matrix (none,
 * hold, reseed, both, and RECOMP_WORKERS=inline) and reports, per variant, whether
 * the [TEST-DRAWS] counts at step 1 agree between two runs and how much of the
 * picture still differs. Each action is logged as [TEST-CROWD].
 */

#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest_call.h"
#include "recorder.h"
#include "test_observations.h"

extern void sub_000AA0D0(void);

#define CROWD_GATE      0x002E3488u
#define SEEDS_MAX       8

static int s_parsed;
static int s_hold, s_hold_n = 880, s_reseed;
static long s_updates;              /* the crowd updates on the game thread */
static int s_in_window, s_held, s_gate_value;
static struct { uint32_t generator, high, low; } s_seeds[SEEDS_MAX];
static int s_seed_count;

static void parse(void)
{
    const char *spec = getenv("RECOMP_TEST_CROWD");
    s_parsed = 1;
    if (!spec || !*spec || !defjam_test_observations_enabled())
        return;
    s_hold = strstr(spec, "hold") != NULL;
    s_reseed = strstr(spec, "reseed") != NULL;
    {
        const char *n = strstr(spec, "hold:");
        if (n && atoi(n + 5) > 0)
            s_hold_n = atoi(n + 5);
    }
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"armed\",\"hold\":%s,\"hold_updates\":%d,\"reseed\":%s}\n",
            s_hold ? "true" : "false", s_hold_n, s_reseed ? "true" : "false");
}

static int active(void)
{
    if (!s_parsed) parse();
    return s_hold || s_reseed;
}

/* sub_001A4D90: the match is being set up. The window starts here. */
void defjam_crowd_match_start(void)
{
    if (!active()) return;
    if (s_held) {                       /* a previous match never reached its first step */
        MEM8(CROWD_GATE) = (uint8_t)s_gate_value;
        s_held = 0;
    }
    s_updates = 0;
    s_seed_count = 0;
    s_in_window = 1;
}

/* sub_000DB870: one update of the background crowd. */
void defjam_crowd_update(void)
{
    long n;
    if (!active() || !s_in_window) return;
    n = ++s_updates;
    if (s_hold && !s_held && n == s_hold_n) {
        /* This update runs; the caller tests the byte before the next one. */
        s_gate_value = MEM8(CROWD_GATE);
        MEM8(CROWD_GATE) = 0;
        s_held = 1;
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"hold\",\"updates\":%ld}\n", (long)n);
    }
}

/* sub_000AA0D0(this = generator, high, low): remembered while the match is set up. */
void defjam_crowd_seed(uint32_t generator, uint32_t stack)
{
    int i;
    if (!active() || !s_in_window || !s_reseed) return;
    for (i = 0; i < s_seed_count; i++)
        if (s_seeds[i].generator == generator) break;
    if (i == s_seed_count) {
        if (s_seed_count == SEEDS_MAX) return;
        s_seed_count++;
    }
    s_seeds[i].generator = generator;
    s_seeds[i].high = MEM32(stack + 4);
    s_seeds[i].low = MEM32(stack + 8);
}

/* sub_00087700: a fight step is about to run. The first one closes the window. */
void defjam_crowd_step(void)
{
    const char *seed_text;
    uint32_t base;
    int i;
    char fields[160];
    if (!active() || !s_in_window) return;
    s_in_window = 0;
    if (s_held) {
        MEM8(CROWD_GATE) = (uint8_t)s_gate_value;
        s_held = 0;
    }
    snprintf(fields, sizeof fields, "\"updates_in_window\":%ld,\"held\":%s,\"reseeded\":%d",
             (long)s_updates, s_hold && s_updates >= s_hold_n ? "true" : "false", s_reseed ? s_seed_count : 0);
    fprintf(stderr, "[TEST-CROWD] {\"event\":\"first_step\",%s}\n", fields);
    recorder_event("crowd", fields);
    if (!s_reseed) return;
    seed_text = getenv("RECOMP_TEST_RNG_SEED");
    base = seed_text && *seed_text ? (uint32_t)strtoul(seed_text, NULL, 0) : 0;
    if (!base)
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"reseed_without_seed\",\"note\":\"set RECOMP_TEST_RNG_SEED; "
                        "the set-up seeds are reused and one of them comes from the time-stamp counter\"}\n");
    for (i = 0; i < s_seed_count; i++) {
        uint32_t args[2];
        args[0] = base ? base + (uint32_t)i * 7919u : s_seeds[i].high;
        args[1] = base ? 0x5EEDu + (uint32_t)i : s_seeds[i].low;
        guest_call(sub_000AA0D0, s_seeds[i].generator, 2, args);
        fprintf(stderr, "[TEST-CROWD] {\"event\":\"reseed\",\"generator\":%u,\"high\":%u,\"low\":%u}\n",
                s_seeds[i].generator, args[0], args[1]);
    }
    fflush(stderr);
}
