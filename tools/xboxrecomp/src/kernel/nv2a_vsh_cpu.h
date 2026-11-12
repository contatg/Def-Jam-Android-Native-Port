/*
 * NV2A vertex programs, run on the CPU for the push-buffer executor.
 *
 * A title whose Direct3D is linked into the executable does not call a
 * runtime Direct3D: it uploads vertex programs and constants through the push
 * buffer and switches the transform unit into program mode. Positions then
 * arrive in the title's own coordinates, and only running the program puts
 * them on the screen. See nv2a_vsh_cpu.c.
 */
#ifndef XBOXRECOMP_NV2A_VSH_CPU_H
#define XBOXRECOMP_NV2A_VSH_CPU_H

#include <stdint.h>

/* What one run of the program produced for one vertex. */
typedef struct {
    float pos[4];    /* oPos: screen x, y, depth, w, as the program left it */
    float d0[4];     /* oD0: diffuse, r g b a in 0..1                       */
    float t0[4];     /* oT0: texture coordinate set 0                       */
} NvVshOutput;

/* 1 when the transform unit is in program mode with a program loaded. */
int nv2a_vsh_active(void);

/* The current value of attribute `attr` (0..15): what the program reads from
 * an input that no vertex array feeds. */
void nv2a_vsh_current(int attr, float out[4]);

/* Run the loaded program on one vertex. `in` is v0..v15. Returns 0 if the
 * program could not be run (none loaded, or it ran off the end). */
int nv2a_vsh_run_ready(const float in[16][4], NvVshOutput *out);

/* Trace the next `runs` vertex runs step by step to stderr (diagnostics). */
void nv2a_vsh_trace(int runs);

/* Which inputs (bit n = v<n>) the current program reads; *writes_consts, if
 * given, says whether it writes any constant register. */
uint32_t nv2a_vsh_inputs_used(int *writes_consts);

/* The current program as HLSL (M4e, docs/research/m4e-gpu-vertex-programs.md):
 * returns its length, or -1 for a program the GPU path does not take. */
int nv2a_vsh_emit_hlsl(char *out, int size, uint32_t *inputs_used);

/* The GPU path's cache key for the current program. */
uint32_t nv2a_vsh_program_hash(void);

/* The 192 constant registers; *version changes whenever one is written. */
const float *nv2a_vsh_constants(uint32_t *version);

#endif
