#ifndef DEFJAM_FG26_POLICY_H
#define DEFJAM_FG26_POLICY_H
#include <stdint.h>
/* Source-interval limits; widened when lsfg33_slow_guard=0 (LSFG only). */
static uint64_t fg26_policy_max_dt=50000000ull,fg26_policy_max_delay=60000000ull;
/* Presentation timestamps only. No guest clock, tick or audio operations. */
typedef struct {
    uint64_t intervals[8],last,delay_ns;
    unsigned at,n,stable,slow;
} FG26Timeline;
static void fg26_timeline_note(FG26Timeline *t,uint64_t stamp) {
    if(t->last && stamp>t->last) {
        uint64_t dt=stamp-t->last;
        if(dt>=14000000ull&&dt<=fg26_policy_max_dt) {
            t->intervals[t->at++%8]=dt;if(t->n<8)t->n++;
            uint64_t largest=0,smallest=UINT64_MAX;
            for(unsigned i=0;i<t->n;i++){if(t->intervals[i]>largest)largest=t->intervals[i];if(t->intervals[i]<smallest)smallest=t->intervals[i];}
            t->stable=largest<=smallest*3/2?t->stable+1:0;
            uint64_t wanted=largest+largest/8+2000000ull;
            if(wanted<20000000ull)wanted=20000000ull;
            if(wanted>fg26_policy_max_delay)wanted=fg26_policy_max_delay;
            /* Delay changes at <=0.5ms/input: avoids moving the timeline back. */
            if(!t->delay_ns)t->delay_ns=wanted;
            else if(wanted>t->delay_ns)t->delay_ns+=wanted-t->delay_ns<500000? wanted-t->delay_ns:500000;
            else t->delay_ns-=t->delay_ns-wanted<500000?t->delay_ns-wanted:500000;
            t->slow=0;
        }else {t->stable=0;t->slow=dt>fg26_policy_max_dt?t->slow+1:0;}
    }
    t->last=stamp;
}
static float fg26_alpha(uint64_t left,uint64_t right,uint64_t target,unsigned stable) {
    if(stable<4||right<=left||right-left>fg26_policy_max_dt||target<=left||target>=right)return -1.0f;
    float a=(float)(target-left)/(float)(right-left);
    return a>0.02f&&a<0.98f?a:-1.0f;
}
#endif
