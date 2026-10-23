/*
 * NV2A vertex program analysis and HLSL emission.
 *
 * A title with its Direct3D linked in drives the transform unit through the
 * push buffer: it switches it to program mode (SET_TRANSFORM_EXECUTION_MODE),
 * uploads microcode (SET_TRANSFORM_PROGRAM, four dwords an instruction) and
 * constants (SET_TRANSFORM_CONSTANT, four floats a register), and draws. The
 * positions it hands over are whatever the program expects -- Def Jam's front
 * end sends stage coordinates, x -2..553 for the legal screen -- and only the
 * program turns them into pixels. Drawn as pixels, every screen landed in the
 * top-left at the wrong scale.
 *
 * Direct3D ends every program with the same epilogue, which is what makes a
 * CPU run enough here: it scales by the viewport and divides by w itself
 * (`MAD oPos.xyz, R12, R1.x, c[59]` after `RCC R1.x, R12.w`), so oPos comes
 * out in screen pixels and needs nothing further.
 *
 * The encoding and the semantics follow xemu's vsh.c (and Cxbx-Reloaded's
 * field table), checked against an instruction this title uploads: dwords
 * 1..3 carry the fields, dword 0 is unused. The toolkit's d3d8_vsh.c decodes
 * a different layout and does not read this title's microcode correctly, so
 * it is not reused.
 *
 * Instructions are decoded once per upload (s_dec) and each runs reading only
 * the operands its opcodes use: a fight runs this for tens of thousands of
 * vertices a frame, and decoding every field of every instruction for each
 * one was two thirds of the GPU executor's time.
 *
 * ponytail: one vertex at a time, and only oPos, oD0 and oT0 are handed on.
 * Multitexture wants the other outputs.
 */

#include <math.h>
#include <stdarg.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_interp.h"

#define VSH_SLOTS      136
#define VSH_CONSTS     192

/* Borrow canonical upload storage; only decoded/HLSL analysis is cached here. */
static const Nv2aVshInstruction *s_prog;
static uint32_t s_prog_start, s_dec_revision;

/* An operand as decoded: where it reads (mux), which register, negation and
 * swizzle. */
typedef struct {
    uint8_t mux, reg, neg, rel;
    uint8_t sw[4];
    uint16_t c;
} VshSrc;

/* An instruction as decoded. `need` says which of A, B, C its opcodes read
 * (bits 0, 1, 2). */
typedef struct {
    uint8_t mac, ilu, mac_mask, ilu_mask, out_r, o_mask, orb, o_from_ilu, final, need;
    uint16_t addr;
    VshSrc src[3];
} VshIns;

static VshIns   s_dec[VSH_SLOTS];
static int      s_dec_valid;       /* s_dec matches s_prog */
static uint32_t s_inputs_start = 0xFFFFFFFFu, s_inputs_mask, s_const_writes;
/* A field of a 128-bit instruction: dword `w`, bit `pos`, `n` bits. */
#define F(ins, w, pos, n) (((ins)[w] >> (pos)) & ((1u << (n)) - 1u))

enum { MUX_R = 1, MUX_V = 2, MUX_C = 3 };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };

static void decode_src(const uint32_t *ins, int which, VshSrc *d)
{
    uint32_t mux, reg, neg, sw[4];
    int i;

    if (which == 0) {        /* A */
        neg = F(ins, 1, 8, 1);
        sw[0] = F(ins, 1, 6, 2); sw[1] = F(ins, 1, 4, 2);
        sw[2] = F(ins, 1, 2, 2); sw[3] = F(ins, 1, 0, 2);
        reg = F(ins, 2, 28, 4);  mux = F(ins, 2, 26, 2);
    } else if (which == 1) { /* B */
        neg = F(ins, 2, 25, 1);
        sw[0] = F(ins, 2, 23, 2); sw[1] = F(ins, 2, 21, 2);
        sw[2] = F(ins, 2, 19, 2); sw[3] = F(ins, 2, 17, 2);
        reg = F(ins, 2, 13, 4);   mux = F(ins, 2, 11, 2);
    } else {                 /* C */
        neg = F(ins, 2, 10, 1);
        sw[0] = F(ins, 2, 8, 2); sw[1] = F(ins, 2, 6, 2);
        sw[2] = F(ins, 2, 4, 2); sw[3] = F(ins, 2, 2, 2);
        reg = (F(ins, 2, 0, 2) << 2) | F(ins, 3, 30, 2);
        mux = F(ins, 3, 28, 2);
    }
    d->mux = (uint8_t)mux;
    d->neg = (uint8_t)neg;
    for (i = 0; i < 4; i++)
        d->sw[i] = (uint8_t)sw[i];
    /* The input register and constant index are shared by all three
     * operands; which one applies depends on the mux. */
    d->reg = (uint8_t)(mux == MUX_V ? F(ins, 1, 9, 4) : (reg < 13 ? reg : 0));
    d->c = (uint16_t)F(ins, 1, 13, 8);
    d->rel = (uint8_t)F(ins, 3, 1, 1);
}

static void decode_all(void)
{
    uint32_t pc;
    for (pc = 0; pc < VSH_SLOTS; pc++) {
        const uint32_t *ins = s_prog[pc];
        VshIns *d = &s_dec[pc];
        int k;
        d->mac = (uint8_t)F(ins, 1, 21, 4);
        d->ilu = (uint8_t)F(ins, 1, 25, 3);
        d->mac_mask = (uint8_t)F(ins, 3, 24, 4);
        d->ilu_mask = (uint8_t)F(ins, 3, 16, 4);
        d->out_r = (uint8_t)F(ins, 3, 20, 4);
        d->o_mask = (uint8_t)F(ins, 3, 12, 4);
        d->orb = (uint8_t)F(ins, 3, 11, 1);
        d->addr = (uint16_t)F(ins, 3, 3, 8);
        d->o_from_ilu = (uint8_t)F(ins, 3, 2, 1);
        d->final = (uint8_t)F(ins, 3, 0, 1);
        for (k = 0; k < 3; k++)
            decode_src(ins, k, &d->src[k]);
        switch (d->mac) {
        case MAC_NOP: d->need = 0; break;
        case MAC_MOV: case MAC_ARL: d->need = 1; break;
        case MAC_ADD: d->need = 1 | 4; break;
        case MAC_MAD: d->need = 1 | 2 | 4; break;
        default: d->need = 1 | 2; break;
        }
        if (d->ilu != ILU_NOP)
            d->need |= 4;
    }
    s_dec_valid = 1;
    s_inputs_start = 0xFFFFFFFFu;
}

static void ensure_decoded(void)
{
    uint32_t revision;
    s_prog = nv2a_vsh_program_data(&revision, &s_prog_start);
    if (!s_dec_valid || revision != s_dec_revision) {
        decode_all();
        s_dec_revision = revision;
    }
}

/* Which inputs (v0..v15) the program from its start reads, and whether it
 * writes a constant: the caller fetches only those attributes, and may reuse
 * a transformed vertex only if nothing it computes feeds the next. */
uint32_t nv2a_vsh_inputs_used(int *writes_consts)
{
    uint32_t pc;
    ensure_decoded();
    if (s_inputs_start != s_prog_start) {
        s_inputs_mask = 0;
        s_const_writes = 0;
        for (pc = s_prog_start; pc < VSH_SLOTS; pc++) {
            const VshIns *d = &s_dec[pc];
            int k;
            for (k = 0; k < 3; k++)
                if ((d->need >> k) & 1 && d->src[k].mux == MUX_V)
                    s_inputs_mask |= 1u << d->src[k].reg;
            if (d->o_mask && !d->orb)
                s_const_writes = 1;
            if (d->final)
                break;
        }
        s_inputs_start = s_prog_start;
    }
    if (writes_consts)
        *writes_consts = (int)s_const_writes;
    return s_inputs_mask;
}

/* The upstream interpreter supplies the richer software output. */
int nv2a_vsh_run_ready(const float in[16][4], NvVshOutput *out)
{
    Nv2aVshOutput full;
    if (!nv2a_vsh_run(in, &full))
        return 0;
    memcpy(out->pos, full.pos, sizeof out->pos);
    memcpy(out->d0, full.d0, sizeof out->d0);
    memcpy(out->t0, full.tex[0], sizeof out->t0);
    return 1;
}

/* ---- The same programs as HLSL, for the GPU (M4e) ----------------------
 *
 * One straight-line block per instruction, with exactly the semantics
 * the established D3D11 path uses. The canonical software interpreter owns
 * uploads and execution; numerical differences must be checked against it.
 * Registers R0..R11, R12 (oPos) and the outputs O[] are float4 arrays; the
 * constants are the 192 registers the title loads (cbuffer b1); a0 is an int.
 *
 * What oPos holds after Direct3D's epilogue is screen pixels in x, y and
 * depth-buffer units in z, with w the clip-space w; the shader turns that
 * back into clip space (x_ndc * w, y_ndc * w, z_01 * w, w) so the hardware
 * clips against the near plane and interpolates with perspective. */

typedef struct {
    char  *p;
    int    left;
    int    bad;
} HlslBuf;

static void hb(HlslBuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (b->left <= 1) {
        b->bad = 1;
        return;
    }
    va_start(ap, fmt);
    n = vsnprintf(b->p, (size_t)b->left, fmt, ap);
    va_end(ap);
    if (n < 0 || n >= b->left) {
        b->bad = 1;
        b->left = 0;
        return;
    }
    b->p += n;
    b->left -= n;
}

/* An operand as an HLSL float4 expression. */
static void hlsl_src(HlslBuf *b, const VshSrc *d)
{
    static const char sw[] = "xyzw";
    const char *neg = d->neg ? "-" : "";
    switch (d->mux) {
    case MUX_R:
        hb(b, "%sR[%u].%c%c%c%c", neg, d->reg, sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]], sw[d->sw[3]]);
        break;
    case MUX_V:
        hb(b, "%sv%u.%c%c%c%c", neg, d->reg, sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]], sw[d->sw[3]]);
        break;
    case MUX_C:
        if (d->rel)
            hb(b, "%sc[((%u + a0 >= 0) && (%u + a0 < %d)) ? %u + a0 : 0].%c%c%c%c", neg, d->c, d->c, VSH_CONSTS, d->c,
               sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]], sw[d->sw[3]]);
        else
            hb(b, "%sc[%u].%c%c%c%c", neg, d->c < VSH_CONSTS ? d->c : 0,
               sw[d->sw[0]], sw[d->sw[1]], sw[d->sw[2]], sw[d->sw[3]]);
        break;
    default:
        hb(b, "float4(0, 0, 0, 0)");
        break;
    }
}

static void hlsl_write(HlslBuf *b, const char *dst, const char *val, uint32_t mask)
{
    if (mask & 8) hb(b, "    %s.x = %s.x;\n", dst, val);
    if (mask & 4) hb(b, "    %s.y = %s.y;\n", dst, val);
    if (mask & 2) hb(b, "    %s.z = %s.z;\n", dst, val);
    if (mask & 1) hb(b, "    %s.w = %s.w;\n", dst, val);
}

/* The current program (from its start to its final instruction) as HLSL.
 * Returns the length, or -1 for a program the GPU path does not take (one
 * that writes a constant, or runs off the end of the program memory). */
int nv2a_vsh_emit_hlsl(char *out, int size, uint32_t *inputs_used)
{
    HlslBuf b;
    uint32_t pc, used;
    int wc = 0, a;

    b.p = out; b.left = size; b.bad = 0;
    used = nv2a_vsh_inputs_used(&wc);
    if (wc)
        return -1;
    if (inputs_used)
        *inputs_used = used;

    hb(&b, "cbuffer VshConsts : register(b1) { float4 c[%d]; };\n", VSH_CONSTS);
    /* x, y: 2 / screen size; z: 1 / zmax; w: unused. TexScale: what a linear
     * texture's texel coordinates are divided by (1 for a swizzled one). */
    hb(&b, "cbuffer VshFrame : register(b2) { float4 Screen; float4 TexScale; };\n");
    hb(&b, "struct VS_IN {\n");
    for (a = 0; a < 16; a++)
        if ((used >> a) & 1)
            hb(&b, "    float4 v%d : ATTR%d;\n", a, a);
    if (!used)
        hb(&b, "    float4 unused : ATTR0;\n");
    hb(&b, "};\n");
    hb(&b, "struct VS_OUT {\n"
           "    float4 pos      : SV_POSITION;\n"
           "    float4 diffuse  : COLOR0;\n"
           "    float4 specular : COLOR1;\n"
           "    float3 tex0     : TEXCOORD0;\n"
           "    float3 tex1     : TEXCOORD1;\n"
           "    float3 tex2     : TEXCOORD2;\n"
           "    float3 tex3     : TEXCOORD3;\n"
           "    float  fog      : TEXCOORD4;\n"
           "    float4 viewpos  : TEXCOORD5;\n"
           "    float  psize    : TEXCOORD6;\n"
           "};\n");
    hb(&b, "VS_OUT main(VS_IN i) {\n");
    for (a = 0; a < 16; a++)
        if ((used >> a) & 1)
            hb(&b, "    float4 v%d = i.v%d;\n", a, a);
    hb(&b, "    float4 R[13];\n    float4 O[13];\n    int a0 = 0;\n"
           "    [unroll] for (int k = 0; k < 13; k++) { R[k] = float4(0, 0, 0, 0); O[k] = float4(0, 0, 0, 0); }\n"
           "    O[3].w = 1.0; O[4].w = 1.0;\n"
           "    O[9].w = 1.0; O[10].w = 1.0; O[11].w = 1.0; O[12].w = 1.0;\n"
           "    float4 A, B, C, m, l;\n");

    if (!s_dec_valid)
        decode_all();
    for (pc = s_prog_start; pc < VSH_SLOTS; pc++) {
        const VshIns *d = &s_dec[pc];
        hb(&b, "    // %u mac %u ilu %u\n", pc, d->mac, d->ilu);
        if (d->need & 1) { hb(&b, "    A = "); hlsl_src(&b, &d->src[0]); hb(&b, ";\n"); }
        if (d->need & 2) { hb(&b, "    B = "); hlsl_src(&b, &d->src[1]); hb(&b, ";\n"); }
        if (d->need & 4) { hb(&b, "    C = "); hlsl_src(&b, &d->src[2]); hb(&b, ";\n"); }
        hb(&b, "    m = float4(0, 0, 0, 0); l = float4(0, 0, 0, 0);\n");
        switch (d->mac) {
        case MAC_MOV: hb(&b, "    m = A;\n"); break;
        case MAC_MUL: hb(&b, "    m = A * B;\n"); break;
        case MAC_ADD: hb(&b, "    m = A + C;\n"); break;
        case MAC_MAD: hb(&b, "    m = A * B + C;\n"); break;
        case MAC_DP3: hb(&b, "    m = dot(A.xyz, B.xyz).xxxx;\n"); break;
        case MAC_DPH: hb(&b, "    m = (dot(A.xyz, B.xyz) + B.w).xxxx;\n"); break;
        case MAC_DP4: hb(&b, "    m = dot(A, B).xxxx;\n"); break;
        case MAC_DST: hb(&b, "    m = float4(1.0, A.y * B.y, A.z, B.w);\n"); break;
        case MAC_MIN: hb(&b, "    m = min(A, B);\n"); break;
        case MAC_MAX: hb(&b, "    m = max(A, B);\n"); break;
        case MAC_SLT: hb(&b, "    m = (A < B) ? 1.0 : 0.0;\n"); break;
        case MAC_SGE: hb(&b, "    m = (A >= B) ? 1.0 : 0.0;\n"); break;
        default: break;
        }
        switch (d->ilu) {
        case ILU_MOV: hb(&b, "    l = C;\n"); break;
        case ILU_RCP: hb(&b, "    l = (C.x == 0.0 ? asfloat(0x7f800000u) : 1.0 / C.x).xxxx;\n"); break;
        case ILU_RCC:
            hb(&b, "    { float r = 1.0 / C.x;"
                   " float q = abs(r); if (q < 5.42101e-20) q = 5.42101e-20;"
                   " if (q > 1.884467e19) q = 1.884467e19; l = (r < 0 ? -q : q).xxxx; }\n");
            break;
        case ILU_RSQ: hb(&b, "    l = (C.x == 0.0 ? asfloat(0x7f800000u) : 1.0 / sqrt(abs(C.x))).xxxx;\n"); break;
        case ILU_EXP:
            hb(&b, "    { float f = floor(C.x); l = float4(exp2(f), C.x - f, exp2(C.x), 1.0); }\n");
            break;
        case ILU_LOG:
            hb(&b, "    { float t = abs(C.x); if (t == 0) l = float4(asfloat(0xff800000u), 1.0, asfloat(0xff800000u), 1.0);"
                   " else { float e = floor(log2(t)); l = float4(e, t / exp2(e), log2(t), 1.0); } }\n");
            break;
        case ILU_LIT:
            hb(&b, "    { float p = clamp(C.w, -127.9961, 127.9961); l = float4(1.0, max(C.x, 0.0),"
                   " C.x > 0 ? pow(max(C.y, 0.0), p) : 0.0, 1.0); }\n");
            break;
        default: break;
        }
        if (d->mac == MAC_ARL) {
            hb(&b, "    a0 = (int)floor(A.x + 0.001);\n");
        } else if (d->mac != MAC_NOP && d->mac_mask && d->out_r < 13) {
            char dst[16];
            snprintf(dst, sizeof dst, "R[%u]", d->out_r);
            hlsl_write(&b, dst, "m", d->mac_mask);
        }
        if (d->ilu != ILU_NOP && d->ilu_mask) {
            uint32_t r = (d->mac != MAC_NOP) ? 1 : d->out_r;
            if (r < 13) {
                char dst[16];
                snprintf(dst, sizeof dst, "R[%u]", r);
                hlsl_write(&b, dst, "l", d->ilu_mask);
            }
        }
        if (d->o_mask && d->orb) {
            const char *val = d->o_from_ilu ? "l" : "m";
            if (d->addr == 0)
                hlsl_write(&b, "R[12]", val, d->o_mask);
            else if (d->addr < 13) {
                char dst[16];
                snprintf(dst, sizeof dst, "O[%u]", d->addr);
                hlsl_write(&b, dst, val, d->o_mask);
            }
        }
        if (d->final)
            break;
    }
    if (pc >= VSH_SLOTS)
        return -1;

    hb(&b, "    VS_OUT o;\n"
           "    float w = R[12].w;\n"
           "    float2 ndc = float2(R[12].x * Screen.x - TexScale.w, 1.0 - R[12].y * Screen.y);\n"
           "    o.pos = float4(ndc * w, R[12].z * Screen.z * w, w);\n"
           "    o.diffuse = saturate(O[3]);\n"
           "    o.specular = saturate(O[4]);\n"
           "    o.tex0 = float3(O[9].xy * TexScale.xy, 0);\n"
           "    o.tex1 = O[10].xyz; o.tex2 = O[11].xyz; o.tex3 = O[12].xyz;\n"
           "    o.fog = 1.0;\n"
           "    o.viewpos = float4(0, 0, 0, 1);\n"
           "    o.psize = O[6].x;\n"
           "    return o;\n"
           "}\n");
    return b.bad ? -1 : (int)(size - b.left);
}

/* A hash of the current program from its start through its final
 * instruction, and the start itself: the GPU path's cache key. */
uint32_t nv2a_vsh_program_hash(void)
{
    uint32_t h, pc;
    int k;
    ensure_decoded();
    h = 2166136261u ^ s_prog_start;
    for (pc = s_prog_start; pc < VSH_SLOTS; pc++) {
        for (k = 1; k < 4; k++)
            h = (h ^ s_prog[pc][k]) * 16777619u;
        if (F(s_prog[pc], 3, 0, 1))
            break;
    }
    return h;
}
