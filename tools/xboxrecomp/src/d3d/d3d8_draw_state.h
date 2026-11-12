#ifndef XBOXRECOMP_D3D8_DRAW_STATE_H
#define XBOXRECOMP_D3D8_DRAW_STATE_H
/* Only omit commands whose exact dynamic state is already bound to this
 * command buffer/pass. Reset after every new pass or command-buffer reset. */
#include <stdint.h>
#include <string.h>
enum {
    DRAW_BIND_SET=1, DRAW_BLEND=2, DRAW_STENCIL_REF=4,
    DRAW_STENCIL_COMPARE=8, DRAW_STENCIL_WRITE=16
};
typedef struct {
    uint64_t set;
    uint32_t offsets[3], stencil[3];
    float blend[4];
    unsigned valid;
} D3D8DrawState;
static inline unsigned d3d8_draw_state_changes(D3D8DrawState *state, uint64_t set,
    const uint32_t offsets[3], const float blend[4], int use_stencil,
    const uint32_t stencil[3])
{
    unsigned changed=0;
    if(!(state->valid&DRAW_BIND_SET)||state->set!=set||memcmp(state->offsets,offsets,sizeof state->offsets)) {
        changed|=DRAW_BIND_SET;state->set=set;memcpy(state->offsets,offsets,sizeof state->offsets);
    }
    if(!(state->valid&DRAW_BLEND)||memcmp(state->blend,blend,sizeof state->blend)) {
        changed|=DRAW_BLEND;memcpy(state->blend,blend,sizeof state->blend);
    }
    if(use_stencil)for(unsigned k=0;k<3;++k) {
        unsigned bit=DRAW_STENCIL_REF<<k;
        if(!(state->valid&bit)||state->stencil[k]!=stencil[k]) {
            changed|=bit;state->stencil[k]=stencil[k];
        }
    }
    state->valid|=changed;
    return changed;
}
#endif
