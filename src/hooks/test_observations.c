/* Experimental read-only combat evidence, enabled by RECOMP_TEST_OBSERVATIONS=1.
 *
 * These addresses/offsets are specific to the verified USA dump. 001A4A30
 * iterates the four 0x12e8-byte fighter records; 001A50B0 changes +350 then
 * notifies 001A6870 with code 0x3d. The HUD consumes +350/+354 as a ratio.
 * The update counter counts this routine's entries, NOT certified simulation
 * frames. A zero-health observation is NOT a KO assertion. Fighter IDs,
 * positions, round state and event-aligned PCM remain to be mapped.
 */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "recomp/gen/recomp_types.h"
#include "test_observations.h"

#define ACTORS 0x003B92E0u
#define STRIDE 0x12E8u
static volatile LONG64 updates;
static HOST_TLS uint32_t pending_actor, pending_caller;
static HOST_TLS float pending_health;

int defjam_test_observations_enabled(void)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *value = getenv("RECOMP_TEST_OBSERVATIONS");
        enabled = value && !strcmp(value, "1");
    }
    return enabled;
}

static int slot(uint32_t actor)
{
    if (actor < ACTORS || actor >= ACTORS + 4 * STRIDE || (actor - ACTORS) % STRIDE)
        return -1;
    return (int)((actor - ACTORS) / STRIDE);
}

static float health(uint32_t actor)
{
    uint32_t bits = MEM32(actor + 0x350);
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

void defjam_test_update(void)
{
    if (defjam_test_observations_enabled())
        InterlockedIncrement64(&updates);
}

void defjam_test_hit(uint32_t attacker, uint32_t defender, uint32_t caller)
{
    static volatile LONG hits;
    int a = slot(attacker), d = slot(defender);
    if (!defjam_test_observations_enabled()) return;
    if (a < 0 || d < 0 || InterlockedIncrement(&hits) > 10000) return;
    fprintf(stderr, "[TEST-OBSERVATION] {\"kind\":\"hit_resolution\",\"update\":%lld,\"host_ms\":%llu,\"attacker_slot\":%d,\"defender_slot\":%d,\"caller\":%u}\n",
            (long long)updates, (unsigned long long)GetTickCount64(), a, d, caller);
    defjam_test_attack(a, d);
}

void defjam_test_health_begin(uint32_t actor, uint32_t caller)
{
    pending_actor = 0;
    if (!defjam_test_observations_enabled() || slot(actor) < 0) return;
    pending_health = health(actor);
    if (!isfinite(pending_health) || pending_health < 0) return;
    pending_actor = actor;
    pending_caller = caller;
}

void defjam_test_health_notify(uint32_t actor, uint32_t code)
{
    static volatile LONG changes;
    float after;
    if (code != 0x3d || actor != pending_actor || !pending_actor) return;
    pending_actor = 0;
    after = health(actor);
    if (!isfinite(after) || after < 0 || after == pending_health || InterlockedIncrement(&changes) > 10000) return;
    fprintf(stderr, "[TEST-OBSERVATION] {\"kind\":\"health_change\",\"update\":%lld,\"host_ms\":%llu,\"slot\":%d,\"before\":%.9g,\"after\":%.9g,\"caller\":%u}\n",
            (long long)updates, (unsigned long long)GetTickCount64(), slot(actor),
            (double)pending_health, (double)after, pending_caller);
    defjam_test_damage(slot(actor), pending_health, after);
}
