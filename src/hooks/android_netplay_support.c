/* Android online profiling/account adapter. Rollback is implemented separately. */
#if defined(__ANDROID__)
#include "rollback.h"
#include "account.h"
#include "netplay.h"
#include "android_netplay.h"
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <stdatomic.h>

static uint64_t online_profile_now(clockid_t clock)
{
    struct timespec ts;
    if (clock_gettime(clock,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000000000ull+(uint64_t)ts.tv_nsec;
}

void android_netplay_profile_step(void)
{
    /* Called on the simulation thread, once in place of netplay_on_step().
     * Wall minus thread CPU includes scheduling/blocking, not just network I/O.
     * Do not change the agreed delay, input history, RNG or checksums here. */
    static _Thread_local uint64_t epoch,prev_end,prev_cpu,hook_wall,hook_cpu,gap_wall,gap_cpu;
    static _Thread_local unsigned steps;
    uint64_t begin=online_profile_now(CLOCK_MONOTONIC);
    uint64_t cpu_begin=online_profile_now(CLOCK_THREAD_CPUTIME_ID);
    if(!begin||!cpu_begin){epoch=prev_end=prev_cpu=0;netplay_on_step();return;}
    if (!epoch || begin<epoch || (prev_end && begin-prev_end>3000000000ull)) {
        epoch=begin;prev_end=prev_cpu=hook_wall=hook_cpu=gap_wall=gap_cpu=0;steps=0;
    }
    if(prev_end&&begin>=prev_end)gap_wall+=begin-prev_end;
    if(prev_cpu&&cpu_begin>=prev_cpu)gap_cpu+=cpu_begin-prev_cpu;
    netplay_on_step();
    uint64_t end=online_profile_now(CLOCK_MONOTONIC);
    uint64_t cpu_end=online_profile_now(CLOCK_THREAD_CPUTIME_ID);
    if(!end||!cpu_end){epoch=prev_end=prev_cpu=0;return;}
    if(end>=begin)hook_wall+=end-begin;
    if(cpu_end>=cpu_begin)hook_cpu+=cpu_end-cpu_begin;
    prev_end=end;prev_cpu=cpu_end;steps++;
    if(epoch&&end>=epoch&&end-epoch>=5000000000ull) {
        fprintf(stderr,"[ANDROID-ONLINE-PERF] %u steps: online hook %.2f ms/step wall, %.2f thread CPU; "
                "between steps %.2f ms wall, %.2f thread CPU; wall minus CPU includes waits/preemption\n",
                steps,(double)hook_wall/1e6/steps,(double)hook_cpu/1e6/steps,
                (double)gap_wall/1e6/steps,(double)gap_cpu/1e6/steps);
        epoch=end;hook_wall=hook_cpu=gap_wall=gap_cpu=0;steps=0;
    }
}
#endif
