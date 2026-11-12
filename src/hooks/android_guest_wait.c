/* Applied only to taken backward branches of the verified MakeSpace SDK
 * function. Keep every original instruction, branch predicate and timeout.
 * Wait only when the executor backlog is real and not making progress. */
#if defined(__ANDROID__)
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>
#include <stdlib.h>
#include "platform/win32_compat.h"
#include "platform/android_hitch.h"
extern uint32_t xbox_Nv2aBacklog(void);
static _Thread_local unsigned polls,unchanged;
static _Thread_local int mode=-1;
static _Thread_local uint32_t previous_backlog;
static _Atomic unsigned pauses,yields;
void defjam_make_space_poll(void) {
    if((++polls&63u)!=0)return;
    if(mode<0){const char *off=getenv("RECOMP_SPACE_BACKOFF");mode=!(off&&*off=='0');}
    if(!mode)return;
    uint32_t backlog=xbox_Nv2aBacklog();
    if(!backlog||backlog!=previous_backlog){previous_backlog=backlog;unchanged=0;return;}
    if(unchanged<4)++unchanged;
    unsigned wait123=dj_trace(DJ_SYNC);
    if(unchanged<4){SwitchToThread();atomic_fetch_add_explicit(&yields,1,memory_order_relaxed);}
    else {
        const struct timespec delay={0,50000};
        nanosleep(&delay,NULL); /* EINTR returns to the original predicate. */
        atomic_fetch_add_explicit(&pauses,1,memory_order_relaxed);
    }
    dj_trace_end(wait123);
}
void defjam_make_space_stats(unsigned *slept,unsigned *yielded){
    *slept=atomic_exchange_explicit(&pauses,0,memory_order_relaxed);
    *yielded=atomic_exchange_explicit(&yields,0,memory_order_relaxed);
}
#endif
