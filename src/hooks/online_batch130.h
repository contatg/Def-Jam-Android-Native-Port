/* Presentation batching only; no prediction or rollback. */
#ifndef DEFJAM_ONLINE_BATCH130_H
#define DEFJAM_ONLINE_BATCH130_H
#include <stdint.h>
static inline unsigned online_batch130_budget(uint64_t local_ns,int cap) {
    uint64_t period=cap>0?1000000000ull/(unsigned)cap:0;
    if(local_ns<period)local_ns=period;
    /* One millisecond tolerance avoids a second update for 16.7 ms jitter. */
    if(local_ns>1000000)local_ns-=1000000;
    unsigned n=local_ns>=66666666ull?4:(unsigned)((local_ns*60+999999999ull)/1000000000ull);
    return n<1?1:n>4?4:n;
}
#endif
