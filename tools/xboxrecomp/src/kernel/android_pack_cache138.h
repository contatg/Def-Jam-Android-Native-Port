#ifndef DEFJAM_PACK_CACHE138_H
#define DEFJAM_PACK_CACHE138_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
/* Render/executor thread only. Never stores a guest pointer for later reads.
 * A hit requires exact keys AND all input bytes (hash is only bucket selection).
 * 16 entries, each <=128KiB input +128KiB output; no unbounded allocation. */
#define PACK138_SLOTS 16
#define PACK138_INPUT (128u*1024u)
#define PACK138_OUTPUT (128u*1024u)
typedef struct {const void *data;size_t size;} PackSegment138;
typedef struct {size_t input,output;uint64_t hash;int valid;unsigned char miss_streak24,cooldown24;unsigned char bytes[PACK138_INPUT+PACK138_OUTPUT];} PackEntry138;
typedef struct {PackEntry138 *entry;uint64_t hits,misses,bypassed;int failed;int adaptive24;uint64_t adaptive_skips24,input_copied24,hit_copied24,avoided_input24,avoided_output24;} PackCache138;
static uint64_t pack_hash138(const void *key,size_t n){
    uint64_t h=14695981039346656037ull;const unsigned char *p=key;
    for(size_t i=0;i<n;i++){h^=p[i];h*=1099511628211ull;}return h;
}
static PackEntry138 *pack_probe138(PackCache138 *c,uint64_t hash,const PackSegment138 *seg,unsigned count,size_t output,void *dst,int *hit){
    size_t total=0;*hit=0;
    for(unsigned i=0;i<count;i++){
        if(seg[i].size>PACK138_INPUT-total||(!seg[i].data&&seg[i].size)){c->bypassed++;return NULL;}
        total+=seg[i].size;
    }
    if(!output||output>PACK138_OUTPUT||c->failed){c->bypassed++;return NULL;}
    if(!c->entry){c->entry=calloc(PACK138_SLOTS,sizeof *c->entry);if(!c->entry){c->failed=1;c->bypassed++;return NULL;}}
    PackEntry138 *e=&c->entry[hash%PACK138_SLOTS];
    /* A dynamic/thrashing bucket is retried after 31 uncached gathers.
     * Bypass never returns cached output: the original gather always runs. */
    if(c->adaptive24 && e->cooldown24){
        --e->cooldown24;c->adaptive_skips24++;c->bypassed++;
        c->avoided_input24+=total;c->avoided_output24+=output;return NULL;
    }
    if(e->valid&&e->hash==hash&&e->input==total&&e->output==output){
        size_t pos=0;int same=1;
        for(unsigned i=0;i<count;i++){if(seg[i].size&&memcmp(e->bytes+pos,seg[i].data,seg[i].size)){same=0;break;}pos+=seg[i].size;}
        if(same){memcpy(dst,e->bytes+total,output);c->hits++;c->hit_copied24+=output;e->miss_streak24=e->cooldown24=0;*hit=1;return e;}
    }
    if(c->adaptive24 && ++e->miss_streak24>=4){e->miss_streak24=0;e->cooldown24=31;}
    c->input_copied24+=total;
    /* Capture input while its render fence still protects it. Commit after all
     * gather workers finish. A changed byte at the same address always misses. */
    e->valid=0;e->hash=hash;e->input=total;e->output=output;
    size_t pos=0;for(unsigned i=0;i<count;i++){if(seg[i].size)memcpy(e->bytes+pos,seg[i].data,seg[i].size);pos+=seg[i].size;}
    c->misses++;return e;
}
static void pack_commit138(PackEntry138 *e,const void *output){if(e){memcpy(e->bytes+e->input,output,e->output);e->valid=1;}}
static void pack_free138(PackCache138 *c){free(c->entry);c->entry=NULL;}
#endif
