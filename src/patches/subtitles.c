#if defined(__ANDROID__)
/* Subtitles in every Story cutscene ([language] story_subtitles, a language pack's
 * story_subtitles=all).
 *
 * USA guest 0x000DA8A0 (thiscall, ecx = the cutscene's subtitle state, one
 * argument: the NIS name) loads a cutscene's subtitle data into [ecx+4] and then
 * decides whether to show it: [ecx+8] = [ecx+4] only when the language index
 * ([0x3412A8]+0x3C) is 3 (French) or the cutscene is assets/nis_sc00.bin, the
 * opening scene. So an English game subtitles the first cutscene and no other,
 * and a translation that keeps the USA language slot would too. ZARF's console
 * patch makes the name compare always succeed (count 0 at 0x000DA901); this does
 * the same after the generated body returns: the function writes [ecx+8] nowhere
 * else, and the subtitle data is already loaded. The Android patch wraps the
 * generated function at build time.
 */
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include "recomp/gen/recomp_types.h"
#include "hooks/pc_language.h"

void defjam_subtitles_after_nis_setup(uint32_t state)
{
    static atomic_uint probes;
    int enabled = pc_language_all_subtitles();
    unsigned entry = atomic_fetch_add_explicit(&probes, 1, memory_order_relaxed);
    if (!state) {
        if (entry < 12) fprintf(stderr, "[LANG40] Story hook: no subtitle state\n");
        return;
    }
    if (!enabled) {
        if (entry < 12) fprintf(stderr, "[LANG40] Story hook: option disabled\n");
        return;
    }
    uint32_t loaded = MEM32(state + 4);
    uint32_t visible = MEM32(state + 8);
    if (loaded && !visible) {
        MEM32(state + 8) = loaded;
        if (entry < 12) fprintf(stderr, "[LANG] cutscene subtitles shown (story_subtitles)\n");
    }
    if (entry < 12)
        fprintf(stderr, "[LANG40] Story hook: data=%d visible_before=%d visible_after=%d\n",
                loaded != 0, visible != 0, MEM32(state + 8) != 0);
}

#endif /* __ANDROID__ */
