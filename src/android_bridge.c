#include "platform/android_hitch.h"
#include "platform/android_power25.h"
#include "platform/android_framegen26.h"
#if defined(__ANDROID__)
#include <jni.h>
#include "d3d8_present.h"
#include "android_frame_pacer.h"
#include "android_input_mode.h"
#include "platform/android_sched.h"
#include "platform/android_thread_policy.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
JNIEXPORT void JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeSetOnlineArtRoot(JNIEnv *env,jclass cls,jstring path) {
    (void)cls;if(!path)return;
    const char *root=(*env)->GetStringUTFChars(env,path,NULL);if(!root)return;
    if(root[0]=='/')setenv("DEFJAM_ONLINE_ART",root,1);
    (*env)->ReleaseStringUTFChars(env,path,root);
}
extern void graphics145_request(void);
extern int graphics145_android_state(int,int);
JNIEXPORT void JNICALL Java_org_defjamffny_android_DefJamActivity_nativeOpenPort145(JNIEnv *env,jclass cls){(void)env;(void)cls;graphics145_request();}
JNIEXPORT jint JNICALL Java_org_defjamffny_android_DefJamActivity_nativeGraphicsState145(JNIEnv *env,jclass cls,jint legacy,jint language){(void)env;(void)cls;return graphics145_android_state(legacy,language);}
static _Atomic int s_frame_limit = -1; /* live override only; -1 delegates to VKSCHED */
static _Atomic int s_module = -1;
static _Atomic int s_guest_cores;
static _Atomic int s_menu_hint;
static _Atomic int s_match_menu;
static _Atomic uint64_t s_fight_step_ns;
static uint64_t input_now_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void defjam_android_note_fight_step(void) {
    atomic_store(&s_fight_step_ns, input_now_ns());
    defjam_monitor_fight_step();
}
static int recent_fight_step(void) {
    uint64_t last = atomic_load(&s_fight_step_ns), now = input_now_ns();
    return last && now >= last && now - last < 250000000ull;
}
void defjam_android_note_pause_menu(unsigned object, unsigned state) {
    int menu = defjam_pause_menu_active(state);
    if (atomic_exchange(&s_match_menu, menu) != menu)
        fprintf(stderr, "[ANDROID-INPUT] pause menu %d object=0x%X state=%u\n", menu, object, state);
}
static _Atomic int s_stable_module = -1;
extern int pc_ui_overlay_open(void);
extern int pc_ui_online_open(void);
extern int pc_ui_online_button(int button, int down);

JNIEXPORT jboolean JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeOnlineMenuButton(JNIEnv *env, jclass type, jint button, jboolean down)
{
    (void)env; (void)type;
    return pc_ui_online_button(button, down == JNI_TRUE) ? JNI_TRUE : JNI_FALSE;
}

void defjam_android_note_module(int type) {
    if (type >= 0 && atomic_exchange(&s_stable_module, type) != type) {
        atomic_store(&s_menu_hint, 0);
        atomic_store(&s_fight_step_ns, 0);
    }
    atomic_store(&s_module, type);
}
void defjam_android_note_frontend_call(const char *text) {
    int hint = defjam_frontend_input_hint(text);
    if (hint >= 0) {
        int menu = !hint;
        if (menu) atomic_store(&s_fight_step_ns, 0);
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
    if (pc_ui_online_open()) return 3;
    if (pc_ui_overlay_open()) return 2;
    module = atomic_load(&s_module);
    int hint = atomic_load(&s_menu_hint), paused = atomic_load(&s_match_menu);
    int active = recent_fight_step();
    int mode = defjam_input_mode_with_fight_step(module, hint, 0, paused, active);
    static _Atomic int reported = -1;
    if (atomic_exchange(&reported, mode) != mode)
        fprintf(stderr, "[ANDROID-INPUT] layout=%d module=%d hint=%d pause=%d live_step=%d\n",
                mode, module, hint, paused, active);
    return mode;
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
    return defjam_input_mode_with_fight_step(module, atomic_load(&s_menu_hint), pc_ui_overlay_open(), atomic_load(&s_match_menu), recent_fight_step()) == 1 && fps>=60;
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
        fprintf(stderr, "[ANDROID-CPU] process %.2f core-equivalents; guest-main allowed %d CPU(s); cap %d FPS; aggregate includes renderer/audio\n",
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
    static uint64_t next;
    static int previous = -1;
    struct timespec now, wait;
    int fps=atomic_load(&s_frame_limit);
    int combat138=defjam_android_combat_active();
    if(fps<0)fps=defjam_sched_limit(combat138);
    else fps=defjam_sched_apply_limit138(fps,combat138);
    defjam_power25_work_end(fps);
    if (fps != previous) {
        next = 0; previous = fps;
        fprintf(stderr, "[ANDROID] frame pacing %d FPS (0=original)\n", fps);
    }
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return;
    android_cpu_report((uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec, fps);
    if (!fps) { next = 0; return; }
    uint64_t current = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
    uint64_t deadline = defjam_frame_deadline_ns(current, next, fps);
    unsigned pace123=dj_trace(DJ_PACE);
    while (current < deadline) {
        wait.tv_sec = (time_t)(deadline / 1000000000ull); wait.tv_nsec = (long)(deadline % 1000000000ull);
        int result = clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&wait,NULL);
        int sleep_errno = result; /* POSIX returns the error number directly. */
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) { next = 0; dj_trace_end(pace123); return; }
        current = (uint64_t)now.tv_sec * 1000000000ull + now.tv_nsec;
        if (result && sleep_errno != EINTR) { next = 0; dj_trace_end(pace123); return; }
    }
    dj_trace_end(pace123);
    next = defjam_frame_next_ns(deadline, current, fps);
}

int defjam_android_combat_active(void) {
    return defjam_input_mode_with_fight_step(atomic_load(&s_module),atomic_load(&s_menu_hint),
        pc_ui_overlay_open(),atomic_load(&s_match_menu),recent_fight_step())==1;
}
void defjam_android_present_observed(uint64_t present_ns,int accepted) {
    if(!accepted)return;
    defjam_perf138_frame();
    struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return;
    int combat=defjam_android_combat_active();
    defjam_power25_present((uint64_t)t.tv_sec*1000000000ull+t.tv_nsec,combat,defjam_sched_limit(combat));
    defjam_monitor_frame((uint64_t)t.tv_sec*1000000000ull+t.tv_nsec,combat,defjam_sched_limit(combat),present_ns);
}
/* Under FG these are real source frames, not generated WSI presents. */
void defjam_android_frame_source26(void) {
    /* Keep real-frame CPU/p95/cadence accounting continuous. No generated
     * output is sent to these counters; present_ns=0 means no source WSI call. */
    defjam_android_present_observed(0,1);
}
JNIEXPORT void JNICALL Java_org_defjamffny_android_DefJamActivity_nativePowerDisplay25(JNIEnv *env,jclass cls,jint req,jint milli,jint mode,jint sustained){(void)env;(void)cls;defjam_power25_display_observed(req,milli,mode,sustained);if(defjam_framegen26_ui())fprintf(stderr,"[FG26-DISPLAY] requested_hz=%d observed_hz=%.3f observed_mode=%d sustained=%d; mode observation is not a physical frame count\n",req,milli/1000.,mode,sustained);}
JNIEXPORT jint JNICALL Java_org_defjamffny_android_DefJamActivity_nativePowerState25(JNIEnv *env,jclass cls){(void)env;(void)cls;int base=defjam_power25_ui(),fg=defjam_framegen26_ui();return fg?(fg|(base&1)):base;}
JNIEXPORT jfloatArray JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeFrameStats(JNIEnv *env,jclass cls) {
    (void)cls;float values[10];defjam_monitor_hud(values);defjam_framegen26_stats(values+6);
    jfloatArray result=(*env)->NewFloatArray(env,10);
    if(result)(*env)->SetFloatArrayRegion(env,result,0,10,values);
    return result;
}
JNIEXPORT void JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeMonitorState(JNIEnv *env,jclass cls,jboolean paused,jint thermal) {
    defjam_framegen26_pause(paused==JNI_TRUE);
    defjam_power25_paused(paused==JNI_TRUE);
    (void)env;(void)cls;defjam_monitor_pause(paused==JNI_TRUE);defjam_monitor_thermal(thermal);
}

JNIEXPORT jfloat JNICALL
Java_org_defjamffny_android_DefJamActivity_nativeGetFps(JNIEnv *env, jclass type)
{
    (void)env;
    (void)type;
    return (jfloat)d3d8_present_fps();
}

JNIEXPORT void JNICALL Java_org_defjamffny_android_DefJamActivity_nativeRuntimeStats(JNIEnv *env,jclass cls,jlong count,jlong ms,jlong bytes){
    (void)env;(void)cls;defjam_trace_runtime(count,ms,bytes);
}


JNIEXPORT void JNICALL Java_org_defjamffny_android_DefJamActivity_nativeTouchSticks(JNIEnv *env,jclass cls,jint lx,jint ly,jint rx,jint ry){
    (void)env;(void)cls;
    extern void xbox_HostInputTouchSticks(int,int,int,int);
    xbox_HostInputTouchSticks(lx,ly,rx,ry);
}
#endif
