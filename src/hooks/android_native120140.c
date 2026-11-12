/* Experimental OFFLINE native frame alarm override. No frame generation.
 * This changes the title clock,not just present. Gameplay speed is UNVALIDATED. */
#if defined(__ANDROID__)
#include "recomp/gen/recomp_types.h"
#include "recomp_settings.h"
#include "platform/android_sched.h"
#include "netplay.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
static pthread_mutex_t clock_lock140=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int enabled140,active140;
static uint32_t owner140;
static uint64_t original140,written140;
static int readable140(uint32_t p,uint32_t n){uint64_t end=(uint64_t)p+n;return (p>=0x1000u&&end<=0x04000000u)||(p>=0x80000000u&&end<=0x84000000u);}
static uint64_t load64140(uint32_t p){return (uint64_t)MEM32(p)|((uint64_t)MEM32(p+4)<<32);}
static void store64140(uint32_t p,uint64_t n){MEM32(p)=(uint32_t)n;MEM32(p+4)=(uint32_t)(n>>32);}
void android_native120_configure140(void){
    int on=0; /* Removed at user request: ignore legacy120 opt-in. */
    atomic_store(&enabled140,on);
    fprintf(stderr,"[NATIVE120-140] enabled=%d; disabled; original game clock retained\n",on);
}
/* Called at the verified entry of the player's generated D7DE0. Context is the
 * game's frame-clock object,not the general kernel timer or audio clock. */
void android_native120_clock140(uint32_t self){
    int enabled=atomic_load_explicit(&enabled140,memory_order_relaxed);
    if(!enabled&&!atomic_load_explicit(&active140,memory_order_relaxed))return;
    if(!readable140(self,0x1AA0u)||self!=MEM32(0x003A0178u))return;
    int wanted=enabled&&defjam_sched_limit(1)==120&&defjam_android_combat_active()&&!netplay_active();
    pthread_mutex_lock(&clock_lock140);
    uint64_t period=load64140(self+0x1A80u),frequency=load64140(self+0x1A98u);
    if(owner140!=self){owner140=self;original140=period;written140=0;}
    /* The title may configure its own timer again. Learn that real period;
     * never halve an already modified period on successive callbacks. */
    if(!written140||period!=written140)original140=period;
    uint64_t target=original140;
    int valid=frequency>=1000&&frequency<=10000000000ull&&original140>0;
    if(valid){uint64_t hz=frequency/original140;valid=(hz>=28&&hz<=32)||(hz>=58&&hz<=62);}
    int active=wanted&&valid;
    if(active)target=frequency/120u;
    if(valid&&target&&target!=period)store64140(self+0x1A80u,target);
    written140=valid?target:period;
    if(atomic_exchange_explicit(&active140,active,memory_order_release)!=active)
        fprintf(stderr,"[NATIVE120-140-CLOCK] active=%d frequency=%llu original_period=%llu applied_period=%llu requested_hz=%d; actual FPS/speed require phone test\n",active,(unsigned long long)frequency,(unsigned long long)original140,(unsigned long long)written140,active?120:0);
    pthread_mutex_unlock(&clock_lock140);
}
int android_native120_flip_hz140(void){return atomic_load_explicit(&enabled140,memory_order_relaxed)&&atomic_load_explicit(&active140,memory_order_acquire)?120:0;}
#endif
