#ifndef DEFJAM_D3D8_FRAMEGEN_H
#define DEFJAM_D3D8_FRAMEGEN_H
/* Independent Xbox/Vulkan prototype inspired by TK3's matrix interpolation.
 * No TK3 implementation is copied. Ordinal + geometry/state signatures are
 * conservative: an unmatched draw stays current; a mostly unmatched frame
 * is never replayed. Native simulation and CPU-transformed HUD are untouched. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define FG_FLOATS (192u * 4u)
#define FG_DRAWS 1024u

typedef struct {
    uint64_t key;
    uint32_t offset;
    int paired, changed;
    float constants[FG_FLOATS];
} FgEntry;
typedef struct {
    FgEntry *previous, *current;
    unsigned previous_n, current_n, paired, moving;
    int failed;
} FgHistory;
static inline uint64_t fg_hash(const void *ptr, size_t n, uint64_t hash)
{
    const unsigned char *p = (const unsigned char *)ptr;
    while(n--) { hash ^= *p++; hash *= 1099511628211ull; }
    return hash;
}
static inline int fg_history_alloc(FgHistory *h)
{
    if (h->previous && h->current) return 1;
    h->previous = (FgEntry *)calloc(FG_DRAWS, sizeof(FgEntry));
    h->current = (FgEntry *)calloc(FG_DRAWS, sizeof(FgEntry));
    if (!h->previous || !h->current) {
        free(h->previous); free(h->current); h->previous=h->current=NULL; return 0;
    }
    return 1;
}
static inline void fg_history_begin(FgHistory *h)
{ h->current_n=h->paired=h->moving=0; h->failed=0; }
static inline void fg_history_record(FgHistory *h, uint64_t key, uint32_t offset, const float *values)
{
    FgEntry *e, *p;
    unsigned n=h->current_n;
    if (!h->current || n>=FG_DRAWS) { h->failed=1; return; }
    e=&h->current[h->current_n++];
    e->key=key; e->offset=offset; e->paired=e->changed=0;
    memcpy(e->constants,values,sizeof e->constants);
    if (n>=h->previous_n || (p=&h->previous[n])->key!=key) return;
    for(unsigned i=0;i<FG_FLOATS;++i) {
        float a=p->constants[i], b=values[i];
        if(!isfinite(a)||!isfinite(b) || fabsf(b-a)>1.0f+0.25f*fmaxf(fabsf(a),fabsf(b))) return;
    }
    e->paired=1; ++h->paired;
    e->changed=memcmp(e->constants,p->constants,sizeof e->constants)!=0;
    h->moving+=e->changed;
}
static inline int fg_history_ready(const FgHistory *h)
{ return !h->failed && h->current_n && h->moving && h->paired*4>=h->current_n*3; }
static inline void fg_history_write(const FgHistory *h, unsigned char *ubo, size_t size, int blend)
{
    for(unsigned n=0;n<h->current_n;++n) {
        const FgEntry *e=&h->current[n];
        float *dst;
        if(e->offset>size || sizeof e->constants>size-e->offset) continue;
        dst=(float *)(ubo+e->offset);
        if(blend && e->paired && e->changed) {
            const float *p=h->previous[n].constants;
            for(unsigned i=0;i<FG_FLOATS;++i)dst[i]=0.5f*p[i]+0.5f*e->constants[i];
        } else memcpy(dst,e->constants,sizeof e->constants);
    }
}
static inline void fg_history_finish(FgHistory *h, int valid)
{
    FgEntry *swap=h->previous;h->previous=h->current;h->current=swap;
    h->previous_n=valid&&!h->failed?h->current_n:0;
    fg_history_begin(h);
}
static inline void fg_history_free(FgHistory *h)
{ free(h->previous);free(h->current);memset(h,0,sizeof *h); }
#endif
