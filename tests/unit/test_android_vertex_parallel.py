"""Host C fixtures: real gather/decoder/pool, no Android SDK or game bytes."""
import os, shutil, subprocess
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]


def test_android_parallel_vertex_output(tmp_path):
    source = tmp_path / 'gather.c'
    source.write_text(r'''
#define _GNU_SOURCE
#define __ANDROID__ 1
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdatomic.h>
static int fail_after = -1, create_calls;
static int checked_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*callback)(void *), void *job) {
    if (fail_after >= 0 && create_calls++ >= fail_after) return EAGAIN;
    return pthread_create(thread, attr, callback, job);
}
#define pthread_create checked_create
#include "nv2a_pb_exec.c"
#undef pthread_create
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }

static atomic_uint seen[12000];
static atomic_uint worker_calls;
static pthread_t dispatcher;
static void count_range(void *unused, uint32_t begin, uint32_t end) {
    (void)unused;
    if(!pthread_equal(dispatcher,pthread_self())) atomic_fetch_add(&worker_calls,1);
    for(uint32_t k=begin;k<end;++k) atomic_fetch_add(&seen[k],1);
}
int main(void) {
    NvVertexPool pool = NV_VERTEX_POOL_INIT;
    dispatcher=pthread_self();
    nv_vertex_pool_run(&pool,8,100,count_range,NULL);
    assert(!pool.initialized && !worker_calls);
    for(unsigned k=0;k<100;++k)assert(seen[k]==1);
    for(unsigned k=0;k<12000;++k)seen[k]=0;
    unsigned parts=0;
    for(unsigned repeat=0;repeat<150;++repeat)
        parts=nv_vertex_pool_run(&pool,8,10000+repeat,count_range,NULL);
    cpu_set_t allowed; assert(sched_getaffinity(0,sizeof allowed,&allowed)==0);
    assert(parts==(unsigned)(CPU_COUNT(&allowed)<8 ? CPU_COUNT(&allowed):8));
    if(parts>1)assert(worker_calls>0);
    for(unsigned k=0;k<10000;++k)assert(seen[k]==150);
    for(unsigned k=10149;k<12000;++k)assert(seen[k]==0);
    nv_vertex_pool_stop(&pool);
    /* Grow after an earlier dispatch, then shrink active participation while
       the persistent helper threads stay alive and asleep. No stale jobs. */
    const unsigned totals[]={2048,8192,2048,4096,9001,100,8192,2049};
    for(unsigned repeat=0;repeat<40;++repeat)for(unsigned j=0;j<8;++j) {
        unsigned total=totals[j];
        for(unsigned k=0;k<12000;++k)seen[k]=0;
        worker_calls=0;
        unsigned actual=nv_vertex_pool_run(&pool,8,total,count_range,NULL);
        unsigned wanted=total<2048?1:total/1024;
        if(wanted>8)wanted=8;
        if(wanted>(unsigned)CPU_COUNT(&allowed))wanted=CPU_COUNT(&allowed);
        assert(actual==wanted);
        assert(worker_calls==actual-1);
        for(unsigned k=0;k<total;++k)assert(seen[k]==1);
        for(unsigned k=total;k<12000;++k)assert(seen[k]==0);
    }
    nv_vertex_pool_stop(&pool);
    /* Partial and total creation failure must keep valid contiguous ranges. */
    for(int fail=0;fail<3;++fail) {
        fail_after=fail; create_calls=0;
        for(unsigned k=0;k<12000;++k)seen[k]=0;
        parts=nv_vertex_pool_run(&pool,8,9001,count_range,NULL);
        assert(parts<=(unsigned)fail+1);
        for(unsigned k=0;k<9001;++k)assert(seen[k]==1);
        nv_vertex_pool_stop(&pool);
    }
    fail_after=-1;
    /* Real production decoder and job, with inline streams covering all
       supported formats, missing attributes and arbitrary index order. */
    static uint32_t data[65536], indices[8192];
    static float serial[8192*64], expected[8192*64], parallel[8192*64];
    NvAndroidGatherJob job = {0};
    const uint32_t types[]={0,1,2,4,5,6,15};
    for(unsigned k=0;k<65536;++k)data[k]=0x3f123456u ^ (k*2654435761u);
    for(unsigned k=0;k<8192;++k)indices[k]=(k*17)%3072;
    job.inline_active=1;job.inline_buf=data;job.inline_count=65536;job.indices=indices;
    for(unsigned a=0;a<16;++a) {
        job.attr[a]=(VertexAttr){.offset=a*4,.type=types[a%7],.size=a%4+1,.stride=64};
        for(unsigned k=0;k<4;++k)job.current[a][k]=(float)(a*10+k);
    }
    job.attr[14].size=0;job.attr[15].offset=0xffffff00u;
    for(unsigned mask_case=0;mask_case<4;++mask_case) {
        job.used=mask_case==0?0:mask_case==1?1:mask_case==2?0x8209:0xffff;
        job.per=0;for(unsigned a=0;a<16;++a)job.per+=(job.used>>a)&1;
        if(!job.per)job.per=1;
        for(unsigned total_case=0;total_case<4;++total_case) {
            unsigned total=total_case==0?17:total_case==1?2047:total_case==2?2048:8192;
            memset(serial,0x5a,sizeof serial);memset(expected,0x5a,sizeof expected);
            memset(parallel,0x5a,sizeof parallel);
            /* Former serial path: expanded 64-float stride, then memmove. */
            for(unsigned v=0;v<total;++v) {
                unsigned o=0;float *dst=serial+(size_t)v*64;
                for(unsigned a=0;a<16;++a)if((job.used>>a)&1) {
                    if(!fetch_attr_from(&job.attr[a],indices[v],dst+o,NULL,1,data,65536,0))
                        memcpy(dst+o,job.current[a],16);
                    o+=4;
                }
                if(!job.used){dst[0]=dst[1]=dst[2]=0;dst[3]=1;}
            }
            for(unsigned v=1;v<total;++v)
                memmove(serial+(size_t)v*job.per*4,serial+(size_t)v*64,job.per*16);
            size_t bytes=(size_t)total*job.per*16;
            memcpy(expected,serial,bytes);
            job.out=parallel;
            for(unsigned fast=0;fast<2;++fast) {
                job.fast=fast;
                memset(parallel,0x5a,sizeof parallel);
                nv_vertex_pool_run(&pool,8,total,android_gather_range,&job);
                assert(memcmp(expected,parallel,bytes)==0);
            }
            /* Range writes must leave the buffer tail untouched. */
            assert(bytes==sizeof parallel || ((unsigned char*)parallel)[bytes]==0x5a);
        }
    }
    nv_vertex_pool_stop(&pool);
    /* Invalid guest pointers read defaults without dereferencing them. */
    float out[4];VertexAttr invalid={.offset=0xbd104f57,.type=2,.size=4,.stride=16};
    assert(!fetch_attr_from(&invalid,0,out,NULL,0,NULL,0,0));
    VertexAttr truncated={.offset=0,.type=2,.size=4,.stride=16};
    assert(!fetch_attr_from(&truncated,0,out,NULL,1,data,1,0));
    /* Span validation must fall back per vertex if even one index is out
       of bounds; it must not turn a readable prefix into all defaults. */
    memset(&job,0,sizeof job);
    job.inline_active=1;job.inline_buf=data;job.inline_count=4;job.indices=indices;
    job.attr[0]=(VertexAttr){.offset=0,.type=2,.size=3,.stride=12};
    job.used=job.per=1;job.out=parallel;indices[0]=0;indices[1]=100;
    for(unsigned k=0;k<4;++k)job.current[0][k]=(float)(17+k);
    job.fast=1;android_gather_range(&job,0,2);
    assert(!memcmp(parallel,data,12)&&parallel[3]==1.0f);
    assert(!memcmp(parallel+4,job.current[0],16));
    /* A float3 at the exact end copies only 12 readable bytes. */
    job.inline_count=3;android_gather_range(&job,0,1);
    assert(!memcmp(parallel,data,12)&&parallel[3]==1.0f);
    /* All float widths, unordered indices and a final partial worker range. */
    memset(&job,0,sizeof job);
    job.inline_active=1;job.inline_buf=data;job.inline_count=65536;
    job.indices=indices;job.used=15;job.per=4;
    for(unsigned a=0;a<4;++a)
        job.attr[a]=(VertexAttr){.offset=a*16,.type=2,.size=a+1,.stride=64};
    for(unsigned v=0;v<8192;++v)indices[v]=(v*17)%3072;
    job.out=expected;job.fast=0;android_gather_range(&job,0,8192);
    job.out=parallel;job.fast=1;
    android_gather_range(&job,0,6144);android_gather_range(&job,6144,8192);
    assert(!memcmp(expected,parallel,8192*4*16));
    /* Adjacent float4 streams may be copied together, without a gap or
       reordering. Cover every constant-size case and the larger fallback. */
    for(unsigned v=0;v<511;++v)indices[v]=(v*17)%511;
    for(unsigned attrs=1;attrs<=5;++attrs) {
        job.per=attrs;job.used=(1u<<attrs)-1;
        for(unsigned a=0;a<attrs;++a)
            job.attr[a]=(VertexAttr){.offset=a*16,.type=2,.size=4,.stride=128};
        job.out=expected;job.fast=0;android_gather_range(&job,0,511);
        job.out=parallel;job.fast=1;android_gather_range(&job,0,511);
        assert(!memcmp(expected,parallel,511*attrs*16));
    }
    job.used=0x15;job.per=3; // sparse attributes must NOT be merged across gaps
    job.out=expected;job.fast=0;android_gather_range(&job,0,511);
    job.out=parallel;job.fast=1;android_gather_range(&job,0,511);
    assert(!memcmp(expected,parallel,511*3*16));
    puts("parallel pool and vertex output: PASS");
}
''')
    out=tmp_path/'gather'
    include=[ROOT/'tools/xboxrecomp/include',ROOT/'tools/xboxrecomp/src',
             ROOT/'tools/xboxrecomp/src/kernel',ROOT/'tools/xboxrecomp/src/platform',
             ROOT/'tools/xboxrecomp/src/d3d',ROOT/'tools/xboxrecomp/src/input',
             ROOT/'tools/xboxrecomp/src/settings']
    command=[shutil.which('cc') or 'cc','-std=gnu11','-O1','-g','-pthread','-D__ANDROID__=1',
             '-ffunction-sections','-fdata-sections', '-fsanitize=address,undefined',
             *['-I'+str(p) for p in include],str(source),str(ROOT/'src/android_thread_policy.c'),
             '-Wl,--gc-sections','-lm','-o',str(out)]
    subprocess.run(command,check=True,capture_output=True,text=True)
    subprocess.run([str(out)],check=True,capture_output=True,text=True,timeout=40,
                   env={**os.environ, "ASAN_OPTIONS":"detect_leaks=0", "RECOMP_NATIVE_CORES":"0"})
