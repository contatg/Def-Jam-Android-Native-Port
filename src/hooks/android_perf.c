/* Opt-in exact inclusive kernel timing. Never changes a wait or a lock.
 * perf-profile.txt: "1" -> 10-second reports, "frame" -> frame reports too.
 * Four clock reads per measured call are intentionally NOT enabled normally. */
#if defined(__ANDROID__)
#include "platform/android_perf.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>
#define PERF_SLOTS 128
typedef struct PerfThread {
    _Atomic long tid;
    _Atomic uint64_t calls[4],wall[4],cpu[4],failures;
} PerfThread;
static PerfThread threads[PERF_SLOTS];
static _Atomic unsigned next_slot;
static _Thread_local int local_slot=-1;
static const unsigned ordinals[4]={277,294,221,234};
int defjam_kernel_profile_mode;
static uint64_t now_ns(clockid_t clock) {
    struct timespec t;
    return clock_gettime(clock,&t)?0:(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;
}
void defjam_perf_init(void) {
    const char *value=getenv("RECOMP_KERNEL_PROFILE");
    defjam_kernel_profile_mode=value&&!strcmp(value,"frame")?2:
        value&&!strcmp(value,"1")?1:0;
    fprintf(stderr,"[KPERF] %s; ordinal counts/time are observations, not reduced guest calls\n",
        defjam_kernel_profile_mode?"exact wall + thread CPU timing enabled (measurement overhead)":"timing off");
}
DefjamKernelProbe defjam_perf_begin(unsigned ordinal) {
    DefjamKernelProbe p={0,0,-1,-1};
    for(int i=0;i<4;i++)if(ordinal==ordinals[i]){p.index=i;break;}
    if(p.index<0)return p;
    if(local_slot==-1) {
        unsigned slot=atomic_fetch_add_explicit(&next_slot,1,memory_order_relaxed);
        local_slot=slot<PERF_SLOTS?(int)slot:-2;
        if(local_slot>=0)atomic_store_explicit(&threads[slot].tid,syscall(SYS_gettid),memory_order_release);
        else fprintf(stderr,"[KPERF] thread registry full; this thread is not timed\n");
    }
    if(local_slot<0)return p;
    p.slot=local_slot;
    atomic_fetch_add_explicit(&threads[p.slot].calls[p.index],1,memory_order_relaxed);
    p.wall=now_ns(CLOCK_MONOTONIC);p.cpu=now_ns(CLOCK_THREAD_CPUTIME_ID);
    return p;
}
void defjam_perf_end(DefjamKernelProbe p) {
    if(p.slot<0||p.index<0)return;
    uint64_t cpu=now_ns(CLOCK_THREAD_CPUTIME_ID),wall=now_ns(CLOCK_MONOTONIC);
    PerfThread *t=&threads[p.slot];
    if(!p.wall||!p.cpu||!wall||!cpu||wall<p.wall||cpu<p.cpu) {
        atomic_fetch_add_explicit(&t->failures,1,memory_order_relaxed);return;
    }
    atomic_fetch_add_explicit(&t->wall[p.index],wall-p.wall,memory_order_relaxed);
    atomic_fetch_add_explicit(&t->cpu[p.index],cpu-p.cpu,memory_order_relaxed);
}
void defjam_perf_frame(uint64_t present_ns,int presented) {
    if(!defjam_kernel_profile_mode)return;
    /* Sole writer: the render executor. Per-thread counters are exchanged
     * atomically; a call spanning a report can finish in the next interval. */
    static uint64_t epoch,sum,max,frames,presents,slow,sequence;
    static uint64_t hist[65]; /* 0.25ms bins; last bin is >=16ms */
    uint64_t now=now_ns(CLOCK_MONOTONIC);
    if(!now)return;
    if(!epoch)epoch=now;
    frames++;sequence++;
    if(presented) {
        presents++;sum+=present_ns;if(present_ns>max)max=present_ns;
        slow+=present_ns>2000000ull;
        unsigned bin=(unsigned)(present_ns/250000ull);if(bin>64)bin=64;hist[bin]++;
    }
    if(defjam_kernel_profile_mode==2)
        fprintf(stderr,"[KPERF-FRAME] frame=%llu present_ms=%.3f image=%d\n",
            (unsigned long long)sequence,present_ns/1e6,presented);
    if(now-epoch<10000000000ull)return;
    uint64_t rank=(presents*95+99)/100,seen=0;unsigned p95=0;
    for(p95=0;p95<65;p95++){seen+=hist[p95];if(seen>=rank)break;}
    fprintf(stderr,"[KPERF] interval=%.3fs frames=%llu presents=%llu present_avg_ms=%.3f p95_upper_ms=%s%.2f max_ms=%.3f over2ms=%llu\n",
        (now-epoch)/1e9,(unsigned long long)frames,(unsigned long long)presents,
        presents?sum/1e6/presents:0,p95>=64?">=":"",p95>=64?16.0:(p95+1)*.25,
        max/1e6,(unsigned long long)slow);
    uint64_t counts[4]={0},walls[4]={0},cpus[4]={0};
    for(unsigned s=0;s<PERF_SLOTS;s++) {
        long tid=atomic_load_explicit(&threads[s].tid,memory_order_acquire);
        if(!tid)continue;
        for(int k=0;k<4;k++) {
            uint64_t calls=atomic_exchange_explicit(&threads[s].calls[k],0,memory_order_relaxed);
            uint64_t wall=atomic_exchange_explicit(&threads[s].wall[k],0,memory_order_relaxed);
            uint64_t cpu=atomic_exchange_explicit(&threads[s].cpu[k],0,memory_order_relaxed);
            counts[k]+=calls;walls[k]+=wall;cpus[k]+=cpu;
            if(calls||wall)fprintf(stderr,"[KPERF-TID] tid=%ld ordinal=%u calls=%llu wall_ms=%.3f cpu_ms=%.3f\n",
                tid,ordinals[k],(unsigned long long)calls,wall/1e6,cpu/1e6);
        }
        uint64_t failed=atomic_exchange_explicit(&threads[s].failures,0,memory_order_relaxed);
        if(failed)fprintf(stderr,"[KPERF] tid=%ld failed_clock_samples=%llu\n",tid,(unsigned long long)failed);
    }
    for(int k=0;k<4;k++)
        fprintf(stderr,"[KPERF-ORD] ordinal=%u calls=%llu calls_per_frame=%.2f wall_ms_per_frame=%.3f cpu_ms_per_frame=%.3f inclusive=1\n",
            ordinals[k],(unsigned long long)counts[k],frames?(double)counts[k]/frames:0,
            frames?walls[k]/1e6/frames:0,frames?cpus[k]/1e6/frames:0);
    fflush(stderr);
    epoch=now;sum=max=frames=presents=slow=0;memset(hist,0,sizeof hist);
}
#endif
