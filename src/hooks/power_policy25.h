#ifndef DEFJAM_POWER_POLICY25_H
#define DEFJAM_POWER_POLICY25_H
#include <stdint.h>
#include <math.h>
/* Android headroom is used thermal envelope, NOT remaining percentage.
 * 1.0 marks the SEVERE threshold. No action on unavailable/NaN readings. */
typedef struct {int level;uint64_t hot_since,cool_since,changed;} PowerPolicy25;
static int power_policy25_step(PowerPolicy25 *p,float head,uint64_t now,int scaled,int allow40){
    int before=p->level;
    if(!isfinite(head)||head<0){p->hot_since=p->cool_since=0;return 0;}
    if(head>=0.85f){
        p->cool_since=0;if(!p->hot_since)p->hot_since=now;
        if(now>=p->hot_since&&now-p->hot_since>=5000000000ull){
            if(p->level==0){p->level=1;p->changed=now;}
            else if(p->level==1&&scaled&&allow40&&now>=p->changed&&now-p->changed>=5000000000ull){p->level=2;p->changed=now;}
        }
    }else if(head<=0.70f){
        p->hot_since=0;if(!p->cool_since)p->cool_since=now;
        if(p->level&&now>=p->cool_since&&now-p->cool_since>=30000000000ull){p->level--;p->changed=p->cool_since=now;}
    }else p->hot_since=p->cool_since=0;
    return before!=p->level;
}
#endif
