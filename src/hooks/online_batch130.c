/* Experimental online presentation batching. OFF unless explicitly selected.
 * Does not enable rollback, alter dt, predict inputs or change the protocol. */
#if defined(__ANDROID__)
#include "online_batch130.h"
#include "netplay.h"
#include "recomp_settings.h"
#include "recomp/gen/recomp_types.h"
#include "platform/android_sched.h"
#include <stdio.h>
#include <time.h>
static _Thread_local struct {
    uint64_t begin,wait,epoch,passes,steps,yields,local_sum,wait_sum;
    unsigned count,budget;
    int active,configured;
} B;
static uint64_t batch_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
void android_online_batch130_begin(uint32_t queue) {
    if(!B.configured)B.configured=recomp_settings_get("vksched","online_batch130",0)==1?2:1;
    if(B.configured==1){B.active=0;return;}
    uint64_t now=batch_now(),wait=defjam_monitor_network_wait_total();
    int active=netplay_active() && netplay_in_fight() && MEM32(queue+0x1A54)==0;
    uint64_t local=0,blocked=0;
    if(active && B.active && now>B.begin && wait>=B.wait && now-B.begin<1000000000ull){
        blocked=wait-B.wait;uint64_t wall=now-B.begin;
        local=blocked<wall?wall-blocked:0;
    }
    B.budget=online_batch130_budget(local,defjam_sched_limit(1));
    B.count=0;B.active=active;B.begin=now;B.wait=wait;
    if(!active){B.epoch=B.passes=B.steps=B.yields=B.local_sum=B.wait_sum=0;return;}
    /* Original D75E0 consumes a fifth input record before returning false.
     * Gate BEFORE that pop; reset only its retry counter at this render pass.
     * Queue size/read index/record data and alternating mode are untouched. */
    MEM32(queue+0x1A5C)=0;
    if(!B.epoch){B.epoch=now;fprintf(stderr,"[NETBATCH130] experimental=1 rollback=0; cap-aware update batches; validate both peers' simulation hashes\n");}
    B.passes++;B.local_sum+=local;B.wait_sum+=blocked;
    if(now>B.epoch && now-B.epoch>=10000000000ull){
        fprintf(stderr,"[NETBATCH130] passes=%llu updates=%llu yields=%llu next_budget=%u local_pass_ms=%.2f network_wait_ms=%.2f; not rollback\n",
            (unsigned long long)B.passes,(unsigned long long)B.steps,(unsigned long long)B.yields,B.budget,
            B.passes?(double)B.local_sum/1e6/B.passes:0,B.passes?(double)B.wait_sum/1e6/B.passes:0);
        B.epoch=now;B.passes=B.steps=B.yields=B.local_sum=B.wait_sum=0;
    }
}
void android_online_batch130_step(void){if(B.active){B.count++;B.steps++;}}
int android_online_batch130_yield(void){
    if(B.active && B.count>=B.budget){B.yields++;return 1;}return 0;
}
#endif
