#ifndef DEFJAM_THERMAL_STATE138_H
#define DEFJAM_THERMAL_STATE138_H
#include <stdint.h>
typedef struct {uint64_t hot_since,cool_since;int limited;} ThermalState138;
/* now is monotonic, each sentinel includes the first qualifying sample. */
static int thermal_step138(ThermalState138 *s,uint64_t now,int state,int milli_c){
    int known=state>=0||milli_c>=0;
    int hot=state>=9||milli_c>=60000;
    int cool=known&&(state<0||state<=6)&&(milli_c<0||milli_c<=55000);
    int old=s->limited;
    if(!s->limited){
        s->cool_since=0;
        if(!hot)s->hot_since=0;
        else if(!s->hot_since)s->hot_since=now;
        else if(now>=s->hot_since&&now-s->hot_since>=5000000000ull){s->limited=1;s->hot_since=0;}
    }else{
        s->hot_since=0;
        if(!cool)s->cool_since=0;
        else if(!s->cool_since)s->cool_since=now;
        else if(now>=s->cool_since&&now-s->cool_since>=15000000000ull){s->limited=0;s->cool_since=0;}
    }
    return old!=s->limited;
}
#endif
