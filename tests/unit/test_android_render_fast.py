"""Production shader lookup/generator and uniform functions, no GPU/FPS claim."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = [ROOT / 'tools/xboxrecomp/include', ROOT / 'tools/xboxrecomp/src',
           ROOT / 'tools/xboxrecomp/src/d3d', ROOT / 'tools/xboxrecomp/src/platform']


def compile_run(tmp, source):
    c = tmp / 'fast.c'
    c.write_text(source)
    binary = tmp / 'fast'
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-ffunction-sections',
                    '-fdata-sections', '-fsanitize=address,undefined',
                    *['-I' + str(p) for p in INCLUDE], str(c), '-Wl,--gc-sections',
                    '-lm', '-o', str(binary)], check=True, capture_output=True, text=True)
    subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})


def test_combiner_key_and_hlsl(tmp_path):
    source = (ROOT / 'tools/xboxrecomp/src/d3d/d3d8_vk.c').read_text()
    begin = source.index('static uint32_t combiner_shader(void)')
    function = source[begin:source.index('/* ── Pipelines', begin)]
    compile_run(tmp_path, r'''
#define __ANDROID__ 1
#include "d3d8_combiners.c"
#include "d3d8_render_fast.h"
#include <assert.h>
#include <stdio.h>
#define COMB_SLOTS 256
typedef struct { int used; NV2ACombinerState st; uint32_t shader; } CombEntry;
static struct { CombEntry combs[COMB_SLOTS]; CombEntry *last_comb; } vk;
static uint64_t s_comb_recent_hits, s_comb_lookups, s_comb_builds;
static unsigned shader_calls;
static int fast;
static int render_fast_enabled(void) { return fast; }
static uint32_t shader_make(const char *text, int vertex, const char *name) {
    assert(text && !vertex && name); ++shader_calls; return 1;
}
#define LOG(...) ((void)0)
''' + function + r'''
int main(void) {
    static char first[32768], next[32768];
    NV2ACombinerState original={0}, code;
    original.num_stages=1;
    original.tex_mode[0]=NV2A_TEXMODE_2D;
    original.stages[0].rgb_input[0].reg=NV2A_REG_C0;
    original.stages[0].rgb_input[1].reg=NV2A_REG_T0;
    assert(d3d8_combiners_generate_hlsl(&original,first,sizeof first)>0);
    g_nv2a_mode=TRUE;
    unsigned counts[2];
    for (fast=0;fast<2;++fast) {
        memset(&vk,0,sizeof vk);shader_calls=0;
        for (unsigned n=0;n<200;++n) {
            g_combiner_state=original;
            for(unsigned s=0;s<8;++s) {
                g_combiner_state.c0[s]=n*(s+1)*17041;
                g_combiner_state.c1[s]=~(n*(s+3)*739);
            }
            g_combiner_state.final_c0=n;g_combiner_state.final_c1=n+100;
            // Changes to a stage NOT read by the program must not pollute its key.
            g_combiner_state.stages[7].rgb_output.ab_dst=(NV2ACombinerRegister)(n%16);
            assert(d3d8_combiners_generate_hlsl(&g_combiner_state,next,sizeof next)>0);
            assert(!strcmp(first,next));
            d3d8_combiner_code_key(&code,&g_combiner_state);
            assert(d3d8_combiners_generate_hlsl(&code,next,sizeof next)>0);
            assert(!strcmp(first,next));
            assert(combiner_shader()==1);
        }
        counts[fast]=shader_calls;
    }
    assert(counts[0]==200 && counts[1]==1);
    assert(s_comb_recent_hits==199);
    fast=1;
    // Active program/texture changes MUST still produce distinct generated code.
    g_combiner_state.stages[0].rgb_input[0].reg=NV2A_REG_V0;
    assert(combiner_shader()==1 && shader_calls==2);
    assert(d3d8_combiners_generate_hlsl(&g_combiner_state,next,sizeof next)>0);
    assert(strcmp(first,next));
    g_combiner_state=original;g_combiner_state.tex_mode[0]=NV2A_TEXMODE_CUBEMAP;
    assert(combiner_shader()==1 && shader_calls==3);
    g_combiner_state=original;assert(combiner_shader()==1 && shader_calls==3);
    // More structures than slots: replacement cannot leave the recent pointer stale.
    for(unsigned n=0;n<COMB_SLOTS+20;++n) {
        g_combiner_state.flags=1000+n;assert(combiner_shader()==1);
    }
    assert(combiner_shader()==1);
    puts("same HLSL/color changes: legacy 200 lookups/builds, fast 1; structural changes preserved");
}
''')


def test_uniform_upload_values_and_lifetimes(tmp_path):
    source = (ROOT / 'tools/xboxrecomp/src/d3d/d3d8_vk.c').read_text()
    begin = source.index('static void color_to_float(')
    functions = source[begin:source.index('/* Everything but the vertex data', begin)]
    compile_run(tmp_path, r'''
#define __ANDROID__ 1
#include "d3d8_render_fast.h"
#include <assert.h>
#include <stdio.h>
typedef uint64_t VkDeviceSize;
typedef struct { unsigned char *map; VkDeviceSize at; } Ring;
static struct { DWORD rs[512]; Ring ubo; int lw,lh,points; float point_size; VkDeviceSize ubo_align; } vk;
static D3D8UniformMemo s_ps_uniform,s_frame_uniform;
static uint64_t s_uniform_copies,s_uniform_reuses,s_uniform_bytes_saved;
static NV2ACombinerState state;
static int fast;
static float s_place_kx=1,s_place_bx;
const NV2ACombinerState *d3d8_combiners_nv2a_state(void) { return &state; }
static int render_fast_enabled(void) { return fast; }
static float d3d8_GetCanvasScaleX(void) { return 1.0f; }
static float d3d8_GetCanvasScaleY(void) { return 1.0f; }
static VkDeviceSize ring_take(Ring *ring, VkDeviceSize size, VkDeviceSize align) {
    ring->at=(ring->at+align-1)/align*align;
    VkDeviceSize at=ring->at;ring->at+=size;assert(ring->at<1024*1024);return at;
}
''' + functions + r'''
int main(void) {
    static unsigned char bytes[1024*1024];
    vk.ubo.map=bytes;vk.ubo_align=256;vk.lw=640;vk.lh=480;
    vk.rs[D3DRS_ALPHAFUNC]=D3DCMP_GREATER;vk.rs[D3DRS_ALPHATESTENABLE]=1;
    for (fast=0;fast<2;++fast) {
        s_ps_uniform.valid=s_frame_uniform.valid=0;
        s_uniform_copies=s_uniform_reuses=0;
        uint32_t ps=0,frame=0;
        for(unsigned n=0;n<100;++n) {
            uint32_t p=ps_constants(),f=frame_constants(1.0f,NULL);
            if(n && fast)assert(p==ps && f==frame);
            ps=p;frame=f;
        }
        assert(s_uniform_copies==(fast?2:200));
        assert(s_uniform_reuses==(fast?198:0));
    }
    fast=1;
    state.c0[0]=0xff123456;
    uint32_t p=ps_constants();
    NV2APSConstants *c=(NV2APSConstants *)(bytes+p);
    assert(c->c0[0][0]==(float)0x12/255.0f && c->c0[0][2]==(float)0x56/255.0f);
    vk.rs[D3DRS_ALPHAREF]=127;
    uint32_t q=ps_constants();assert(p!=q);
    assert(((NV2APSConstants *)(bytes+q))->alpha_ref==(float)127/255.0f);
    uint32_t f=frame_constants(0.0f,NULL);
    assert(frame_constants(-0.0f,NULL)!=f); // byte equality, not approximate floats
    f=frame_constants(1.0f,NULL);s_place_bx=.25f;
    assert(frame_constants(1.0f,NULL)!=f);
    p=ps_constants();s_ps_uniform.valid=s_frame_uniform.valid=0;
    assert(ps_constants()!=p); // new command-buffer lifetime needs its own upload
    puts("uniform bytes preserved; 200 -> 2 copies for identical draws; reset/change force new upload");
}
''')
