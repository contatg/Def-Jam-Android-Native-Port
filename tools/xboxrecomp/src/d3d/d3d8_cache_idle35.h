#ifndef DEFJAM_D3D8_CACHE_IDLE35_H
#define DEFJAM_D3D8_CACHE_IDLE35_H
#include <stdint.h>
/* An isolated inactive observation isn't enough: a lagging fight may stop
 * reporting steps briefly. Require a continuous quiet period before doing
 * synchronous driver cache extraction or file writes on the render thread. */
typedef struct { uint64_t since; int armed; } CacheIdleGate35;
static inline int cache_idle_ready35(CacheIdleGate35 *g,uint64_t now,int combat,uint64_t delay_ms){
    if(combat){g->armed=0;return 0;}
    if(!g->armed||now<g->since){g->since=now;g->armed=1;return 0;}
    return now-g->since>=delay_ms;
}
#endif
