"""Host tests of production v7 pacing and affinity code; no game/Android SDK needed."""
import os
import shutil
import subprocess
from pathlib import Path
import pytest
ROOT = Path(__file__).resolve().parents[2]
CC = shutil.which('cc')
pytestmark = pytest.mark.skipif(not CC or os.name != 'posix', reason='POSIX C compiler required')


def build(tmp, source, *flags):
    src = tmp / 'check.c'
    src.write_text(source)
    out = tmp / 'check'
    subprocess.run([CC, '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                    str(src), '-o', str(out)], check=True, capture_output=True, text=True)
    return out


def test_guest_masks_and_actual_affinity(tmp_path):
    source = (ROOT / 'tools/xboxrecomp/src/kernel/kernel_bridge.c').read_text()
    start = source.index('void xbox_PinToGuestCore(void)')
    function = source[start:source.index('\nstatic DWORD WINAPI bridge_thread_main', start)]
    out = build(tmp_path, r'''
#define _GNU_SOURCE
#define __ANDROID__ 1
#include "android_guest_cores.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
typedef long LONG;
static LONG InterlockedCompareExchange(volatile LONG *p, LONG value, LONG expected) {
    LONG old = *p; if (old == expected) *p = value; return old;
}
static void InterlockedExchange(volatile LONG *p, LONG value) { *p = value; }
#define YieldProcessor() ((void)0)
static int effective;
void defjam_android_note_guest_cores(int count) { effective = count; }
''' + function + r'''
int main(int argc, char **argv) {
    cpu_set_t allowed, got, expected, sparse;
    assert(argc == 2);
    CPU_ZERO(&sparse); CPU_SET(1, &sparse); CPU_SET(4, &sparse); CPU_SET(7, &sparse);
    assert(defjam_select_guest_cores("2", &sparse, &expected) == 2);
    assert(CPU_ISSET(7, &expected) && CPU_ISSET(4, &expected) && !CPU_ISSET(1, &expected));
    assert(defjam_select_guest_cores("8", &sparse, &expected) == 3);
    assert(defjam_select_guest_cores("all", &sparse, &expected) == 3);
    const char *bad[] = {NULL,"0","9","-1","2x","","99999999999999999"};
    for (unsigned i=0; i<sizeof bad/sizeof bad[0]; ++i) {
        assert(defjam_select_guest_cores(bad[i], &sparse, &expected) == 1);
        assert(CPU_ISSET(7, &expected));
    }
    CPU_ZERO(&sparse); assert(defjam_select_guest_cores("8", &sparse, &expected) == 0);
    assert(sched_getaffinity(0, sizeof allowed, &allowed) == 0);
    int count = defjam_select_guest_cores(argv[1], &allowed, &expected);
    assert(setenv("RECOMP_GUEST_CORES", argv[1], 1) == 0);
    xbox_PinToGuestCore();
    assert(sched_getaffinity(0, sizeof got, &got) == 0);
    assert(CPU_EQUAL(&expected, &got));
    assert(CPU_COUNT(&got) == count);
    assert(effective == count);
    /* A child inherits a restricted mask; entering the bridge must restore
       the full configured subset, including after parent affinity changes. */
    cpu_set_t one; CPU_ZERO(&one);
    int highest=CPU_SETSIZE-1; while(highest>=0 && !CPU_ISSET(highest,&expected))--highest;
    CPU_SET(highest,&one);
    assert(sched_setaffinity(0,sizeof one,&one)==0);
    xbox_PinToGuestCore();
    assert(sched_getaffinity(0,sizeof got,&got)==0);
    assert(CPU_EQUAL(&expected,&got));
    return 0;
}
''', '-I' + str(ROOT / 'tools/xboxrecomp/src/kernel'))
    for count in ('1', '2', '3', '4', '5', '6', '7', '8', 'all'):
        subprocess.run([str(out), count], check=True, capture_output=True, text=True)


def test_native_frame_pacer_and_live_toggle(tmp_path):
    # JNI and presenter types only: timing and atomic implementation is production code.
    (tmp_path / 'jni.h').write_text('typedef void JNIEnv; typedef void *jclass; typedef float jfloat; typedef int jint;\n#define JNIEXPORT\n#define JNICALL\n')
    (tmp_path / 'd3d8_present.h').write_text('float d3d8_present_fps(void);\n')
    out = build(tmp_path, r'''
#define _POSIX_C_SOURCE 200809L
#define __ANDROID__ 1
#include <assert.h>
#include <stdint.h>
#include <errno.h>
#include <time.h>
static uint64_t clock_ns = 1000000000ull;
static uint64_t slept;
static int interrupt_once;
static int fake_clock_gettime(clockid_t clock, struct timespec *value) {
    (void)clock; value->tv_sec=clock_ns/1000000000ull; value->tv_nsec=clock_ns%1000000000ull; return 0;
}
static int fake_nanosleep(const struct timespec *req, struct timespec *remaining) {
    uint64_t ns=(uint64_t)req->tv_sec*1000000000ull+req->tv_nsec;
    if(interrupt_once) { interrupt_once=0; *remaining=*req; errno=EINTR; return -1; }
    clock_ns+=ns; slept+=ns; return 0;
}
static int overlay_open;
int pc_ui_overlay_open(void) { return overlay_open; }
int defjam_android_native_core_count(void) { return 8; }
void nv2a_android_vertex_stats_report(void) { }
float d3d8_present_fps(void) { return 42.0f; }
#define clock_gettime fake_clock_gettime
#define nanosleep fake_nanosleep
#include "android_bridge.c"
int main(void) {
    assert(defjam_frame_wait_ns(100, 0, 30) == 0);
    assert(defjam_frame_wait_ns(100, 200, 30) == 0);
    assert(defjam_frame_wait_ns(100000000, 1, 30) == 0);
    assert(defjam_frame_wait_ns(16000001, 1, 0) == 0);
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetFps(0,0) == 42);
    defjam_android_note_guest_cores(5);
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetGuestCoreCount(0,0) == 5);
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetNativeCoreCount(0,0) == 8);
    for(int m=-1;m<8;++m) {
        defjam_android_note_module(m);
        int want=m==2||m==3||m==6?1:0;
        assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==want);
    }
    overlay_open=1;
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==2);
    overlay_open=0;
    /* Exact observed flow: module2 contains both the style menu and fight.
       A loading tick (-1) and repeated stable ticks must not lose the menu. */
    defjam_android_note_module(2);
    defjam_android_note_frontend_call("Game.ShowFightingMovie(1)");
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==0);
    defjam_android_note_module(-1);defjam_android_note_module(2);
    defjam_android_note_frontend_call("Game.ChooseFightingStyle(0)");
    defjam_android_note_frontend_call("Game.ShowFightingMovie(0)");
    defjam_android_note_frontend_call("Audio.Play(10)");
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==0);
    overlay_open=1;assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==2);
    overlay_open=0;
    defjam_android_note_frontend_call("GAME.ENTERTUTORIALMODE()");
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==1);
    defjam_android_note_frontend_call("Game.ShowFightingMovie(1)");
    defjam_android_note_module(0);defjam_android_note_module(3);
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(0,0)==1);
    Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,30);
    defjam_android_pace_present(); assert(slept == 0); // first frame
    clock_ns+=16000000ull; interrupt_once=1;
    defjam_android_pace_present(); assert(slept == 17333333ull); // 33.333 ms interval, EINTR retry
    clock_ns+=50000000ull;
    defjam_android_pace_present(); assert(slept == 17333333ull); // slow frame: no extra sleep
    clock_ns+=5000000000ull;
    defjam_android_pace_present(); assert(slept == 17333333ull); // resume: no catch-up
    Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,0);
    defjam_android_pace_present(); defjam_android_pace_present(); assert(slept == 17333333ull);
    Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,30);
    defjam_android_pace_present(); assert(slept == 17333333ull); // re-enable resets baseline
    const int rates[] = {30,31,45,60,90,120,144,165};
    for (unsigned i=0; i<sizeof rates/sizeof rates[0]; ++i) {
        int fps = rates[i];
        Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,0);
        defjam_android_pace_present();
        Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,fps);
        defjam_android_pace_present();
        uint64_t before = slept;
        defjam_android_pace_present();
        assert(slept-before == 1000000000ull/(unsigned)fps);
    }
    assert(defjam_frame_limit_value("30") == 30);
    assert(defjam_frame_limit_value("165") == 165);
    const char *invalid[] = {0,"","0","29","166","90x","9999999999999999999999999"};
    for (unsigned i=0; i<sizeof invalid/sizeof invalid[0]; ++i)
        assert(defjam_frame_limit_value(invalid[i]) == 0);
    assert(defjam_frame_wait_ns(200,100,166) == 0);
    Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,120);
    defjam_android_note_module(2);assert(defjam_android_framegen_allowed());
    overlay_open=1;assert(!defjam_android_framegen_allowed());overlay_open=0;
    defjam_android_note_module(0);assert(!defjam_android_framegen_allowed());
    defjam_android_note_module(2);
    Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(0,0,30);
    assert(!defjam_android_framegen_allowed());
    defjam_android_report_framegen(0);
    clock_ns+=1000000000ull;defjam_android_report_framegen(1);
    clock_ns+=1000000000ull;defjam_android_report_framegen(1);
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetGeneratedFps(0,0)==1.0f);
    clock_ns+=2000000000ull;defjam_android_report_framegen(0);
    assert(Java_org_defjamffny_android_DefJamActivity_nativeGetGeneratedFps(0,0)==0.0f);

    return 0;
}
''', '-I' + str(tmp_path), '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'tools/xboxrecomp/src'))
    subprocess.run([str(out)], check=True, capture_output=True, text=True)
