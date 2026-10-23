"""Exercise the production fence and GPU interrupt loops with a fake clock."""
from pathlib import Path
import os
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(path, signature):
    source = path.read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', start) + 2]


def test_android_gpu_wait_predicates_and_timeouts(tmp_path):
    fence = function(ROOT / 'src/recomp_manual.c', 'void sub_0021EB90_enter(void)')
    interrupt = function(ROOT / 'tools/xboxrecomp/src/kernel/kernel_bridge.c',
                         'static void kernel_gpu_preempt(void)')
    fixture = tmp_path / 'gpu_wait.c'
    fixture.write_text(r'''
#define _POSIX_C_SOURCE 200809L
#define __ANDROID__ 1
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
typedef int32_t LONG;
typedef int64_t LONG64;
typedef int64_t LONGLONG;
typedef struct { int64_t QuadPart; } LARGE_INTEGER;
static uint64_t now_ns = 1000000000ull, ready_at;
static unsigned yields, sleeps, interrupted;
static int interrupt_once, pending;
static uint32_t backlog;
static uint32_t memory[0x240000 / 4];
#define MEM32(a) memory[(a) / 4]
static void update(void) {
    if (ready_at && now_ns >= ready_at) {
        MEM32(0x20000) = MEM32(0x10000 + 0x2c);
        pending = 0; backlog = 0;
    }
}
static void SwitchToThread(void) { ++yields; now_ns += 10000; update(); }
static int fake_nanosleep(const struct timespec *req, struct timespec *remaining) {
    (void)remaining;
    assert(req->tv_sec == 0 && req->tv_nsec == 100000);
    ++sleeps;
    if (interrupt_once) {
        interrupt_once = 0; ++interrupted; now_ns += 10000; update();
        errno = EINTR; return -1;
    }
    now_ns += (uint64_t)req->tv_nsec; update(); return 0;
}
#define nanosleep fake_nanosleep
#include "android_gpu_wait.h"
static void QueryPerformanceCounter(LARGE_INTEGER *p) { p->QuadPart = now_ns; }
static void QueryPerformanceFrequency(LARGE_INTEGER *p) { p->QuadPart = 1000000000; }
static LONG InterlockedIncrement(volatile LONG *p) { return ++*p; }
static LONG64 InterlockedAdd64(volatile LONG64 *p, LONG64 v) { return *p += v; }
static int kernel_nv2a_swm_waiting(void) { return pending; }
static uint32_t xbox_Nv2aBacklog(void) { return backlog; }
static int kernel_gpu_behind(int holding) { return backlog > (holding ? 0x20000u : 0x40000u); }
static int preemptible = 1;
static int xbox_IrqlPreemptible(void) { return preemptible; }
static void *g_timer_wake;
static void SetEvent(void *event) { (void)event; }
''' + fence + '\n' + interrupt + r'''
static void begin(uint64_t delay) {
    now_ns += 1000000000ull; yields = sleeps = 0;
    ready_at = delay ? now_ns + delay : 0;
    MEM32(0x234768) = 0x10000;
    MEM32(0x10000 + 0x30) = 0x20000;
    MEM32(0x10000 + 0x2c) = 32; MEM32(0x20000) = 0;
    pending = 1; backlog = 0x50000u;
}
int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) {
        assert(!setenv("RECOMP_FENCE_HOLD", "0", 1));
        assert(!setenv("RECOMP_GPU_PREEMPT", "0", 1));
        begin(0); sub_0021EB90_enter(); kernel_gpu_preempt();
        assert(!yields && !sleeps); return 0;
    }
    begin(0); MEM32(0x20000) = 32; pending = 0; backlog = 0;
    sub_0021EB90_enter(); kernel_gpu_preempt(); assert(!yields && !sleeps);
    begin(30000); sub_0021EB90_enter();
    assert(yields && !sleeps && MEM32(0x20000) == 32);
    begin(20000000); interrupt_once = 1; uint64_t start = now_ns;
    sub_0021EB90_enter();
    assert(sleeps && interrupted == 1 && yields < 300);
    assert(MEM32(0x20000) == 32 && now_ns >= ready_at && now_ns - ready_at < 120000);
    assert(now_ns - start < 20200000);
    begin(20000000); kernel_gpu_preempt();
    assert(sleeps && yields < 300 && !pending && !backlog);
    assert(now_ns >= ready_at && now_ns - ready_at < 120000);
    begin(0); preemptible = 0; kernel_gpu_preempt(); assert(!sleeps && !yields);
    preemptible = 1;
    begin(0); start = now_ns; sub_0021EB90_enter();
    assert(now_ns - start > 100000000 && now_ns - start < 100200000);
    assert(MEM32(0x20000) == 0);  /* Timeout does not complete a fence. */
    begin(0); start = now_ns; kernel_gpu_preempt();
    assert(now_ns - start > 50000000 && now_ns - start < 50200000);
    assert(pending && backlog == 0x50000u);
    yields = sleeps = 0; kernel_gpu_preempt();
    assert(!yields && !sleeps);  /* Original one-second deadlock escape remains. */
    puts("GPU readiness, short/long waits, EINTR, timeouts and disabled paths: PASS");
}
''')
    out = tmp_path / 'gpu_wait'
    subprocess.run([shutil.which('cc') or 'cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'tools/xboxrecomp/src/kernel'), str(fixture), '-o', str(out)],
                   check=True, capture_output=True, text=True)
    for arguments in ([], ['disabled']):
        subprocess.run([str(out), *arguments], check=True, capture_output=True, text=True,
                       timeout=5, env={k: v for k, v in os.environ.items()
                                       if k not in ('RECOMP_FENCE_HOLD', 'RECOMP_GPU_PREEMPT')})
