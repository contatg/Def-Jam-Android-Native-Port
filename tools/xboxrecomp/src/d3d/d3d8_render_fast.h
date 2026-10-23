#ifndef D3D8_RENDER_FAST_H
#define D3D8_RENDER_FAST_H

/* Render-state deltas, adapted to the C/NV2A backend. Black Android's
 * MakePixelKey classifies shader code separately from DrawConstants.
 * Numeric colors are uniform data, not part of generated HLSL. */
#include "d3d8_combiners.h"
#include <stdint.h>
#include <string.h>

static inline void d3d8_combiner_code_key(NV2ACombinerState *key,
                                    const NV2ACombinerState *state)
{
    *key = *state;
    memset(key->c0, 0, sizeof key->c0);
    memset(key->c1, 0, sizeof key->c1);
    key->final_c0 = key->final_c1 = 0;
    /* The generator reads only active stages. Keep malformed counts intact:
     * this key is not responsible for repairing the input program. */
    if (key->num_stages >= 0 && key->num_stages < NV2A_MAX_COMBINER_STAGES)
        memset(&key->stages[key->num_stages], 0,
               (NV2A_MAX_COMBINER_STAGES - key->num_stages) * sizeof key->stages[0]);
}

/* The uploaded range remains immutable for the current command-buffer
 * lifetime. Invalidate at every frame_begin, including overflow/recovery.
 * Compare bytes, so signed zero and NaN payloads are not silently changed. */
typedef struct {
    unsigned char bytes[sizeof(NV2APSConstants)];
    uint32_t offset;
    size_t size;
    int valid;
} D3D8UniformMemo;

static inline int d3d8_uniform_same(const D3D8UniformMemo *memo, const void *bytes, size_t size)
{
    return memo->valid && memo->size == size && size <= sizeof memo->bytes
        && !memcmp(memo->bytes, bytes, size);
}

static inline void d3d8_uniform_remember(D3D8UniformMemo *memo, const void *bytes,
                                   size_t size, uint32_t offset)
{
    memo->valid = 0;
    if (size > sizeof memo->bytes) return;
    memcpy(memo->bytes, bytes, size);
    memo->size = size;
    memo->offset = offset;
    memo->valid = 1;
}
#endif
