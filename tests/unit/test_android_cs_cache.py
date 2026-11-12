"""Real RTL shadow lookup and POSIX recursive locks, including contention."""
from pathlib import Path
import os
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def test_shadow_cache_retains_recursive_and_shared_lock_semantics(tmp_path):
    source = tmp_path / 'cs.c'
    source.write_text(r'''
#define _GNU_SOURCE
#include "kernel.h"
#include <pthread.h>
#include <assert.h>
#include <stdatomic.h>
#include <stdarg.h>
static _Thread_local unsigned reads;
static void counted_read(PSRWLOCK lock) { ++reads; AcquireSRWLockShared(lock); }
#define AcquireSRWLockShared counted_read
#include "kernel_rtl.c"
#undef AcquireSRWLockShared
ptrdiff_t g_xbox_mem_offset;
RECOMP_TLS uint32_t g_esp, g_xbox_kernel_caller;
uint32_t g_xbox_code_lo,g_xbox_code_hi;
static RTL_CRITICAL_SECTION guest[128];
static unsigned total;
void xbox_log(int level,const char *category,const char *format,...) {
    (void)level;(void)category;(void)format;
}
static void *worker(void *unused) {
    (void)unused;
    for(unsigned i=0;i<4000;++i) {
        xbox_RtlEnterCriticalSection(&guest[0]);
        xbox_RtlEnterCriticalSection(&guest[0]);
        unsigned before=total;
        if(!(i&63))sched_yield();
        total=before+1;
        xbox_RtlLeaveCriticalSection(&guest[0]);
        xbox_RtlLeaveCriticalSection(&guest[0]);
    }
    return NULL;
}
int main(void) {
    xbox_RtlInitializeCriticalSection(&guest[0]);
    CRITICAL_SECTION *original=xbox_cs_shadow(&guest[0]);assert(original);
    assert(!xbox_cs_shadow(NULL));
    unsigned before=reads;
    for(unsigned i=0;i<20000;++i) {
        xbox_RtlEnterCriticalSection(&guest[0]);
        xbox_RtlLeaveCriticalSection(&guest[0]);
    }
    if(!g_cs_single_mode)assert(reads-before==(g_cs_cache_on?0:40000));
    // Collisions and eviction must resolve to the same stable host lock.
    for(unsigned repeat=0;repeat<3;++repeat)for(unsigned n=0;n<128;++n) {
        xbox_RtlInitializeCriticalSection(&guest[n]);
        xbox_RtlEnterCriticalSection(&guest[n]);xbox_RtlLeaveCriticalSection(&guest[n]);
    }
    assert(xbox_cs_shadow(&guest[0])==original);
    pthread_t threads[8];
    for(unsigned i=0;i<8;++i)assert(!pthread_create(&threads[i],NULL,worker,NULL));
    for(unsigned i=0;i<8;++i)assert(!pthread_join(threads[i],NULL));
    assert(total==8*4000 && original->RecursionCount==0);
    assert(g_cs_enters==g_cs_leaves);
}
''')
    binary = tmp_path / 'cs'
    include = [ROOT / 'tools/xboxrecomp/include', ROOT / 'tools/xboxrecomp/src',
               ROOT / 'tools/xboxrecomp/src/kernel', ROOT / 'tools/xboxrecomp/src/platform']
    subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-pthread', '-D__ANDROID__=1',
                    '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                    *['-I'+str(p) for p in include], str(source),
                    str(ROOT / 'tools/xboxrecomp/src/platform/win32_compat.c'),
                    '-Wl,--gc-sections', '-lm', '-o', str(binary)],
                   check=True, capture_output=True, text=True)
    for fast in ('0', '1'):
        for mode in ('normal', 'single'):
            subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30,
                           env={**os.environ, 'RECOMP_CPU_FAST': fast, 'RECOMP_CS_MODE': mode,
                                'ASAN_OPTIONS': 'detect_leaks=0'})
