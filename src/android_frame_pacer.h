#ifndef DEFJAM_ANDROID_FRAME_PACER_H
#define DEFJAM_ANDROID_FRAME_PACER_H
#include <stdint.h>
#include <stdlib.h>
/* No catch-up loop after a slow frame or resume; disabled means zero wait. */
static inline int defjam_frame_limit_value(const char *value)
{
    char *end;
    long fps;
    if (!value || !*value) return 0;
    fps = strtol(value, &end, 10);
    return *end == 0 && fps >= 30 && fps <= 165 ? (int)fps : 0;
}
static inline uint64_t defjam_frame_wait_ns(uint64_t now, uint64_t last, int fps)
{
    uint64_t period, elapsed;
    if (fps < 30 || fps > 165 || !last || now < last) return 0;
    period = 1000000000ull / (unsigned)fps;
    elapsed = now - last;
    return elapsed < period ? period - elapsed : 0;
}
/* Keep a stable phase through ordinary sleep overshoot, without a burst of
 * catch-up frames after slow rendering, a changed cap, or background/resume. */
static inline uint64_t defjam_frame_deadline_ns(uint64_t now, uint64_t next, int fps)
{
    if (fps < 30 || fps > 165 || !next) return now;
    uint64_t period = 1000000000ull / (unsigned)fps;
    if(next<now)return now-next<period?next:now;
    return next-now<=period?next:now;
}
static inline uint64_t defjam_frame_next_ns(uint64_t deadline, uint64_t woke, int fps)
{
    if (fps < 30 || fps > 165) return 0;
    uint64_t period = 1000000000ull / (unsigned)fps;
    uint64_t base = woke >= deadline && woke - deadline >= period ? woke : deadline;
    return base > UINT64_MAX - period ? 0 : base + period;
}
#endif
