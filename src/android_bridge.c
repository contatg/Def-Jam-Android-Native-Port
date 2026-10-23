#if defined(__ANDROID__)
#include <jni.h>
#include "d3d8_present.h"
#include "android_frame_pacer.h"
#include "android_input_mode.h"
#include "platform/android_thread_policy.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
static _Atomic int s_frame_limit = -1;
static _Atomic int s_module = -1;
static _Atomic int s_guest_cores;
static _Atomic int s_menu_hint;
static _Atomic int s_stable_module = -1;
extern int pc_ui_overlay_open(void);

void defjam_android_note_module(int type) {
    if (type >= 0 && atomic_exchange(&s_stable_module, type) != type
            && type != 2 && type != 3 && type != 6) {
        atomic_store(&s_menu_hint, 0);
    }
    atomic_store(&s_module, type);
}
void defjam_android_note_frontend_call(const char *text) {
    int hint = defjam_frontend_input_hint(text);
    if (hint >= 0) {
        int menu = !hint;
        if (atomic_exchange(&s_menu_hint, menu) != menu)
            fprintf(stderr, "[ANDROID-INPUT] %s controls after %.170s\n", menu ? "menu" : "fight", text);
    }
}
void defjam_android_note_guest_cores(int count) { atomic_store(&s_guest_cores, count); }

JNIEXPORT jint JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeGetInputMode(JNIEnv *env, jclass type)
{
    int module;
    (void)env; (void)type;
    if (pc_ui_overlay_open()) return 2;
    module = atomic_load(&s_module);
    return defjam_input_mode(module, atomic_load(&s_menu_hint), 0);
}
JNIEXPORT jint JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeGetNativeCoreCount(JNIEnv *env, jclass type)
{
    (void)env; (void)type;
    return defjam_android_native_core_count();
}
JNIEXPORT jint JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeGetGuestCoreCount(JNIEnv *env, jclass type)
{
    (void)env; (void)type;
    return atomic_load(&s_guest_cores);
}

/* Native game-module context: generated images are disabled in menus/overlays. */
int defjam_android_framegen_allowed(void)
{
    int module=atomic_load(&s_module), fps=atomic_load(&s_frame_limit);
    if(fps<0)fps=defjam_frame_limit_value(getenv("DEFJAM_FPS_LIMIT"));
    return defjam_input_mode(module, atomic_load(&s_menu_hint), pc_ui_overlay_open()) == 1 && fps>=60;
}
int defjam_android_framegen_limit(void)
{
    int fps=atomic_load(&s_frame_limit);
    return fps<0?defjam_frame_limit_value(getenv("DEFJAM_FPS_LIMIT")):fps;
}

static _Atomic int s_generated_millifps;
void defjam_android_report_framegen(int generated)
{
    static uint64_t first, frames;
    struct timespec ts; uint64_t now;
    if(clock_gettime(CLOCK_MONOTONIC,&ts)!=0)return;
    now=(uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec;
    frames+=generated!=0;
    if(!first){first=now;frames=0;return;}
    if(now-first>=2000000000ull){
        int value=(int)(frames*1000000000000ull/(now-first));
        atomic_store(&s_generated_millifps,value);
        if(value)fprintf(stderr,"[FRAMEGEN] %.1f additional generated FPS; base FPS counted separately\n",(double)value/1000.0);
        frames=0;first=now;
    }
}
JNIEXPORT jfloat JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeGetGeneratedFps(JNIEnv *env,jclass type)
{
    (void)env;(void)type;return (jfloat)atomic_load(&s_generated_millifps)/1000.0f;
}

static void android_cpu_report(uint64_t now, int cap)
{
    extern void nv2a_android_vertex_stats_report(void);
    static uint64_t last_wall, last_cpu;
    struct timespec cpu;
    uint64_t ns;
    if (last_wall && now - last_wall < 2000000000ull) return;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) != 0) return;
    nv2a_android_vertex_stats_report();
    ns = (uint64_t)cpu.tv_sec * 1000000000ull + cpu.tv_nsec;
    if (last_wall && now > last_wall && ns >= last_cpu)
        fprintf(stderr, "[ANDROID-CPU] process %.2f core-equivalents, guest mask %d CPU(s), cap %d FPS; process includes renderer/audio\n",
                (double)(ns - last_cpu) / (double)(now - last_wall), atomic_load(&s_guest_cores), cap);
    last_wall = now; last_cpu = ns;
}

JNIEXPORT void JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeSetFrameLimit(JNIEnv *env, jclass type, jint fps)
{
    (void)env; (void)type;
    atomic_store(&s_frame_limit, fps >= 30 && fps <= 165 ? fps : 0);
}

void defjam_android_pace_present(void)
{
    static uint64_t last;
    static int previous = -1;
    struct timespec now, wait;
    uint64_t ns;
    int fps = atomic_load(&s_frame_limit);
    if (fps < 0) {
        const char *value = getenv("DEFJAM_FPS_LIMIT");
        int expected = -1;
        atomic_compare_exchange_strong(&s_frame_limit, &expected,
                defjam_frame_limit_value(value));
        fps = atomic_load(&s_frame_limit);
    }
    if (fps != previous) {
        last = 0; previous = fps;
        fprintf(stderr, "[ANDROID] frame pacing %d FPS (0=original)\n", fps);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    android_cpu_report((uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec, fps);
    if (!fps) { last = 0; return; }
    ns = defjam_frame_wait_ns((uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec, last, fps);
    wait.tv_sec = (time_t)(ns / 1000000000ull); wait.tv_nsec = (long)(ns % 1000000000ull);
    while (ns && nanosleep(&wait, &wait) != 0 && errno == EINTR) {}
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        last = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
}

JNIEXPORT jfloat JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeGetFps(JNIEnv *env, jclass type)
{
    (void)env;
    (void)type;
    return (jfloat)d3d8_present_fps();
}
#endif
