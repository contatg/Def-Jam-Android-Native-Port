#ifndef D3D8_CPU146_H
#define D3D8_CPU146_H
/* Host render-owner caches. Equality, not the bucket hash, decides reuse.
 * Inspired by MostWanted's recent-shader and unchanged-constant fast paths.
 * No guest-memory lifetime, shader arithmetic or Vulkan synchronization changes. */
#include "d3d8_combiners.h"
#include <stdint.h>
#include <string.h>

typedef struct {
    uint32_t c0[NV2A_MAX_COMBINER_STAGES], c1[NV2A_MAX_COMBINER_STAGES];
    uint32_t final_c0, final_c1, alpha_ref, alpha_func, alpha_enable;
} D3D8PSKey146;

static inline void d3d8_ps_key146(D3D8PSKey146 *key,
                                 const NV2ACombinerState *st,
                                 uint32_t ref, uint32_t func, uint32_t enable)
{
    if (st) {
        memcpy(key->c0, st->c0, sizeof key->c0);
        memcpy(key->c1, st->c1, sizeof key->c1);
        key->final_c0 = st->final_c0;
        key->final_c1 = st->final_c1;
    } else {
        memset(key->c0, 0, sizeof key->c0);
        memset(key->c1, 0, sizeof key->c1);
        key->final_c0 = key->final_c1 = 0;
    }
    key->alpha_ref = ref & 255u;
    key->alpha_func = func;
    key->alpha_enable = enable != 0;
}

/* memcpy keeps unaligned input and strict aliasing safe. Every byte participates;
 * the complete original state is still compared at each matching cache slot. */
static inline uint32_t d3d8_state_hash146(const void *data, size_t size)
{
    const unsigned char *p = (const unsigned char *)data;
    uint32_t hash = 0x9E3779B9u ^ (uint32_t)size;
    while (size >= 4) {
        uint32_t word;
        memcpy(&word, p, sizeof word);
        hash ^= word * 0x85EBCA6Bu;
        hash = ((hash << 13) | (hash >> 19)) * 0xC2B2AE35u;
        p += 4;
        size -= 4;
    }
    while (size--) hash = (hash ^ *p++) * 16777619u;
    hash ^= hash >> 16;
    return hash;
}
#endif
