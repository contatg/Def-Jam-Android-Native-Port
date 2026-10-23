#ifndef DEFJAM_ANDROID_GPU_WAIT_H
#define DEFJAM_ANDROID_GPU_WAIT_H

/* Call only inside an existing predicate/timeout loop, with SwitchToThread
 * already declared. Keep short handoffs responsive; when the GPU/executor
 * stays behind, stop a guest thread consuming a core on sched_yield().
 * EINTR deliberately returns to the caller so it rechecks readiness/timeout.
 * No fence is released, skipped or signalled here. */
#include <stdint.h>
#include <time.h>
static inline void android_gpu_wait_pause(uint64_t elapsed_ns)
{
    // Keep the runtime's guest handoff, including optional guest-turn mode.
    SwitchToThread();
    if (elapsed_ns >= 250000u) {
        const struct timespec pause = { 0, 100000L };
        nanosleep(&pause, NULL);
    }
}
#endif
