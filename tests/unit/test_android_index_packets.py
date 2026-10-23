"""Real parser, production index handlers and legacy translator accounting."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]
INCLUDE = [ROOT / 'tools/xboxrecomp/include', ROOT / 'tools/xboxrecomp/src',
           ROOT / 'tools/xboxrecomp/src/kernel', ROOT / 'tools/xboxrecomp/src/platform',
           ROOT / 'tools/xboxrecomp/src/d3d', ROOT / 'tools/xboxrecomp/src/nv2a']


def compile_fixture(tmp, code, name):
    source = tmp / (name + '.c')
    source.write_text(code)
    binary = tmp / name
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-D__ANDROID__=1',
                    '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                    *['-I'+str(p) for p in INCLUDE], str(source),
                    '-Wl,--gc-sections', '-lm', '-o', str(binary)],
                   check=True, capture_output=True, text=True)
    return binary


def test_index_packets_match_scalar_parser_and_partial_put(tmp_path):
    source = (ROOT / 'tools/xboxrecomp/src/kernel/nv2a_pb_exec.c').read_text()
    begin = source.index('int nv2a_pb_exec_index_packet(')
    fast = source[begin:source.index('\n#endif', begin)]
    begin = source.index('    case NV097_ARRAY_ELEMENT16:')
    scalar = source[begin:source.index('    case NV097_DRAW_ARRAYS:', begin)]
    binary = compile_fixture(tmp_path, r'''
#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#define NV097_ARRAY_ELEMENT16 0x1800u
#define NV097_ARRAY_ELEMENT32 0x1808u
#define NV_MAX_INDICES 32768u
static struct { uint32_t prim,idx_count,idx[NV_MAX_INDICES]; } s_gpu;
static uint64_t s_index_bulk_packets,s_index_bulk_words,scalar_calls;
static uint32_t forwarded,ignored;
static int s_exec_inited,s_ftrace;
static int verbose;
static int pb_exec_verbose(void){return verbose;}
static int pb_d3d11_forwarding(void){return 1;}
static void pgraph_d3d11_note_index_words(uint32_t n){forwarded+=n;ignored+=n;}
''' + fast + r'''
#include "nv2a_pb_scan.c"
static uint32_t memory[131072],at;
static uint32_t fences,stalls;
ptrdiff_t xbox_GetMemoryOffset(void){return (ptrdiff_t)(uintptr_t)memory-XBOX_CONTIG_BASE;}
void xbox_Nv2aReleasePump(int at_put){(void)at_put;}
void nv2a_pb_exec_report(void){}
void nv2a_pb_exec_method(uint32_t subch,uint32_t method,uint32_t param) {
    ++scalar_calls;s_exec_inited=1;
    if(method==0x1800||method==0x1808)pgraph_d3d11_note_index_words(1);
    if(subch)return;
    switch(method) {
''' + scalar + r'''
    case 0x17fc:s_gpu.prim=param;break;
    case 0x1d70:++fences;break;
    case 0x0100:if(param){++stalls;nv2a_pb_stall();}break;
    }
}
static uint32_t header(uint32_t method,uint32_t count,int noninc,uint32_t subch) {
    uint32_t pos=at;memory[at++]=(noninc?0x40000000u:0)|(count<<18)|(subch<<13)|method;
    return pos;
}
static uint32_t address(uint32_t word){return XBOX_CONTIG_BASE+word*4;}
static void run(uint32_t *cursor,uint32_t end) {
    int stopped=0;unsigned budget=1000;
    while(*cursor!=end) {
        uint32_t previous=*cursor;
        *cursor=nv2a_pb_run(*cursor,end,&stopped);
        assert(--budget && (*cursor!=previous || stopped));
        if(stopped)assert(stalls);
    }
}
int main(void) {
    setenv("RECOMP_PB_EXEC","1",1);
    uint32_t params[8]={0};
    assert(!nv2a_pb_exec_index_packet(0,0x1800,params,8)); // no initialization yet
    // Empty draw, then an active draw with both index widths and all fallbacks.
    header(0x1800,8,1,0);for(unsigned i=0;i<8;++i)memory[at++]=i;
    header(0x17fc,1,0,0);memory[at++]=5;
    header(0x1800,1024,1,0);for(unsigned i=0;i<1024;++i)memory[at++]=(i<<16)|(i+1);
    header(0x1800,4,0,0);for(unsigned i=0;i<4;++i)memory[at++]=i+19;
    header(0x1800,9,1,2);for(unsigned i=0;i<9;++i)memory[at++]=i+29;
    header(0x1808,65,1,0);for(unsigned i=0;i<65;++i)memory[at++]=i+39;
    uint32_t split=header(0x1800,96,1,0);
    for(unsigned i=0;i<96;++i)memory[at++]=(i<<16)|(i+49);
    header(0x1d70,1,0,0);memory[at++]=0xdeadbeef;
    header(0x0100,1,0,0);memory[at++]=5;
    // Force capacity overflow: it must fall back with the same partial tail.
    for(unsigned j=0;j<20;++j) {
        header(0x1800,1024,1,0);
        for(unsigned i=0;i<1024;++i)memory[at++]=(i<<16)|(i+59+j);
    }
    header(0x17fc,1,0,0);memory[at++]=0;
    header(0x1808,8,1,0);for(unsigned i=0;i<8;++i)memory[at++]=0xff00+i;
    // CALL/RETURN/JUMP around index packets must retain the walk and fences.
    uint32_t call=at++;
    header(0x1808,8,1,0);for(unsigned i=0;i<8;++i)memory[at++]=0xee00+i;
    uint32_t jump=at++,sub=at;
    header(0x1800,12,1,0);for(unsigned i=0;i<12;++i)memory[at++]=i;
    memory[at++]=0x00020000u; // RETURN
    uint32_t tail=at;
    header(0x1d70,1,0,0);memory[at++]=0xcafef00d;
    memory[call]=address(sub)|2u;
    memory[jump]=address(tail)|1u;
    uint32_t cursor=address(0),saved_count=0;
    const uint32_t cuts[]={split+1,split+4,split+12};
    for(unsigned i=0;i<3;++i) {
        run(&cursor,address(cuts[i]));
        assert(s_pend.count==96-(cuts[i]-(split+1)));
        saved_count+=s_pend.count;
    }
    run(&cursor,address(at));
    assert(!s_pend.count && fences==2 && stalls==1 && s_tot_jumps==3);
    assert(s_gpu.idx_count==NV_MAX_INDICES && s_gpu.idx[0]==1 && s_gpu.idx[1]==0);
    uint64_t hash=1469598103934665603ull;
    for(unsigned n=0;n<s_gpu.idx_count;++n)hash=(hash^s_gpu.idx[n])*1099511628211ull;
    for(int n=0;n<s_seen_count;++n)hash=(hash^s_seen[n].count^s_seen[n].method)*1099511628211ull;
    printf("RESULT %llu %u %u %u %u %u %u %u\n",(unsigned long long)hash,s_gpu.idx_count,
           forwarded,ignored,saved_count,g_pb_words,g_pb_live_va,s_tot_words);
    int enabled=strcmp(getenv("RECOMP_CPU_FAST"),"0")!=0;
    if(enabled)assert(s_index_bulk_packets && s_index_bulk_words>14000 && scalar_calls<15000);
    else assert(!s_index_bulk_packets && scalar_calls>20000);
    assert(!nv2a_pb_exec_index_packet(1,0x1800,params,8));
    assert(!nv2a_pb_exec_index_packet(0,0x17fc,params,8));
    s_ftrace=2;assert(!nv2a_pb_exec_index_packet(0,0x1800,params,8));s_ftrace=0;
    verbose=1;assert(!nv2a_pb_exec_index_packet(0,0x1800,params,8));verbose=0;
    assert(!pb_index_packet(0,0x1800,1,0x83fffff8u,0x84000020u,8));
    assert(!pb_index_packet(0,0x1800,1,XBOX_CONTIG_BASE-4,XBOX_CONTIG_BASE+32,8));
}
''', 'packets')
    results = []
    for fast in ('0', '1'):
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30,
                                env={**os.environ, 'RECOMP_CPU_FAST': fast, 'ASAN_OPTIONS': 'detect_leaks=0'})
        results.append(next(line for line in result.stdout.splitlines() if line.startswith('RESULT ')))
    assert results[0] == results[1]


def test_bulk_translator_counters_equal_original_index_methods(tmp_path):
    binary = compile_fixture(tmp_path, r'''
#include "nv2a_pgraph_d3d11.c"
#include <assert.h>
IDirect3DDevice8 *xbox_GetD3DDevice(void){return NULL;}
int main(void) {
    for(unsigned initialized=0;initialized<2;++initialized) {
        memset(&g_pg,0,sizeof g_pg);g_pg.initialized=initialized;
        g_pg.in_draw=1;g_pg.inline_count=3;
        for(unsigned i=0;i<2047;++i)pgraph_d3d11_method(0,0x1800,i);
        for(unsigned i=0;i<65;++i)pgraph_d3d11_method(0,0x1808,i);
        unsigned handled=g_pg.stats.methods_handled,ignored=g_pg.stats.methods_ignored;
        assert(g_pg.in_draw==1&&g_pg.inline_count==3);
        memset(&g_pg.stats,0,sizeof g_pg.stats);
        pgraph_d3d11_note_index_words(2047);pgraph_d3d11_note_index_words(65);
        assert(handled==g_pg.stats.methods_handled&&ignored==g_pg.stats.methods_ignored);
        assert(g_pg.in_draw==1&&g_pg.inline_count==3);
    }
}
''', 'accounting')
    subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                   env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'})
