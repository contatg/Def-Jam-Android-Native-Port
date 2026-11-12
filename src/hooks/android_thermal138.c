#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "platform/android_sched.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
static uint64_t now138(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
/* ABI compatibility with cumulative138. Even an old INI value1 cannot enable
 * a worker,thermal read,cap or clock change. OS thermal protections stay native. */
void defjam_thermal138_init(int enabled){(void)enabled;fprintf(stderr,"[THERM139] application thermal ceiling removed; legacy thermal_adaptive ignored\n");}
int defjam_thermal138_limit(int cap,int combat,int cadence_blocked){(void)combat;(void)cadence_blocked;return cap;}
void defjam_thermal138_report(void){fprintf(stderr,"[THERM139] application_ceiling=none worker=none; hardware diagnostics only\n");}
/* Each gather batch contributes its existing measured duration. Relaxed atomics
 * are counters only; no memory ownership/synchronization depends on them. */
static _Atomic uint64_t gather_ns138,batches138,frames138,perf_begin138;
void defjam_perf138_frame(void){uint64_t empty=0;atomic_compare_exchange_strong(&perf_begin138,&empty,now138());atomic_fetch_add_explicit(&frames138,1,memory_order_relaxed);}
static _Atomic unsigned helpers138;
void defjam_perf138_gather(uint64_t ns,unsigned helpers){atomic_fetch_add_explicit(&gather_ns138,ns,memory_order_relaxed);atomic_fetch_add_explicit(&batches138,1,memory_order_relaxed);atomic_store_explicit(&helpers138,helpers,memory_order_relaxed);}
void defjam_perf138_report(void){
    uint64_t ns=atomic_exchange(&gather_ns138,0),n=atomic_exchange(&batches138,0);
    static uint64_t previous;uint64_t now=now138(),frames=atomic_exchange(&frames138,0);
    uint64_t begin=previous?previous:atomic_load(&perf_begin138);
    double seconds=begin&&now>begin?(now-begin)/1e9:0;previous=now;
    fprintf(stderr,"[PERF138-10S] interval_s=%.3f fps=%.3f gather_pack_ms_frame=%.4f gather_pack_ms_total=%.3f gather_batches=%llu frames=%llu helpers_created=%u; accepted-present FPS (not scanout); per-thread CPU%% in CPU121\n",
        seconds,seconds?frames/seconds:0,frames?ns/1e6/frames:0,ns/1e6,(unsigned long long)n,(unsigned long long)frames,atomic_load(&helpers138));
}
#endif
