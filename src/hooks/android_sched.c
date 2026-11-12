/* Presentation policy only. Never edits a guest step, clock, RNG or netplay packet. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "platform/android_sched.h"
#include "platform/android_hitch.h"
#include "recomp_settings.h"
#include "android_rollback131.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sched.h>
#include "platform/android_power25.h"
static _Atomic int low_end, selected=-1, menu_limit=60;
static int profile, vertex_setting;
static _Atomic int auto30_blocked;
static unsigned slow_step_windows;
static int valid_limit(int n) { return n==0||n==30||n==40||n==60||n==120; }
static int cpu_low_end(void) {
    cpu_set_t allowed; unsigned long max=0, values[CPU_SETSIZE]={0}; int n=0,big=0;
    if(sched_getaffinity(0,sizeof allowed,&allowed))return 0;
    for(int i=0;i<CPU_SETSIZE;i++)if(CPU_ISSET(i,&allowed)) {
        char p[160];snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",i);
        FILE *f=fopen(p,"r");if(!f)return 0;
        int ok=fscanf(f,"%lu",&values[i])==1;fclose(f);if(!ok||!values[i])return 0;
        if(values[i]>max)max=values[i];
        n++;
    }
    for(int i=0;i<CPU_SETSIZE;i++)if(values[i]&&values[i]*100>=max*95)big++;
    return n==8&&big==4&&max<=2500000; /* A conservative 4+4/frequency heuristic, not proof of IPC. */
}
static int adreno6(const char *s) {
    char lower[256];size_t n=0;
    if(!s)return 0;
    while(s[n]&&n+1<sizeof lower){lower[n]=(char)tolower((unsigned char)s[n]);n++;}lower[n]=0;
    char *p=strstr(lower,"adreno");if(!p)return 0;
    while(*p&&!isdigit((unsigned char)*p))p++;
    return p[0]=='6'&&isdigit((unsigned char)p[1])&&isdigit((unsigned char)p[2]);
}
void defjam_sched_settings_ready(void) {
    extern void graphics145_ready(void);
    graphics145_ready();
    android_rollback131_configure(recomp_settings_get("vksched","rollback131",0));
    profile=recomp_settings_get("vksched","profile",0);
    { const char *keys[]={"fsr144","screen_filter144","anisotropy144","shader_preload144","fsr_sharpness144","precise_barriers144","spatial34"};
      const char *envs[]={"RECOMP_VK_FSR144","RECOMP_VK_FILTER144","RECOMP_VK_ANISO144","RECOMP_VK_PRELOAD144","RECOMP_VK_SHARPNESS144","RECOMP_VK_BARRIERS144","RECOMP_VK_SPATIAL34"};
      const int defaults[]={0,0,1,1,80,1,0};
      for(unsigned i=0;i<7;i++){char value[24];snprintf(value,sizeof value,"%d",recomp_settings_get("graphics",keys[i],defaults[i]));setenv(envs[i],value,1);}
    }
    setenv("DEFJAM_PACK_ADAPT24",recomp_settings_get("vksched","adaptive_pack24",1)?"1":"0",1);
    setenv("DEFJAM_PROG_INDEX24",recomp_settings_get("vksched","program_index24",1)?"1":"0",1);
    setenv("DEFJAM_FIFO22",recomp_settings_get("vksched","fifo22",0)?"1":"0",1);
    {
        int on25=recomp_settings_get("vksched","power25",1);char value[24];
        snprintf(value,sizeof value,"%d",on25?recomp_settings_get("vksched","present_mode25",0):0);setenv("DEFJAM_PRESENT_MODE25",value,1);
        snprintf(value,sizeof value,"%d",on25?recomp_settings_get("vksched","swap_images25",0):0);setenv("DEFJAM_SWAP_IMAGES25",value,1);
    }
    setenv("DEFJAM_KERNEL_LOG26",recomp_settings_get("vksched","kernel_log26",0)?"1":"0",1);
    int engine39 = recomp_settings_get("graphics", "fg_engine39", 0);
    setenv("DEFJAM_FRAMEGEN39", engine39 == 2 ? "2" : engine39 == 1 ? "1" : "0", 1);
    fprintf(stderr, "[FG39] selected engine=%d (0=off, 1=Win-FG, 2=Lossless); Win-FG in combat, LSFG in menus, cutscenes and combat\n", engine39);
    {char value[24];const char *keys[]={"lsfg27_flow","lsfg27_fp16","lsfg27_gpu_budget_us","lsfg30_multiplier","lsfg30_refresh","lsfg32_height","lsfg27_budget_guard","lsfg27_warmup_samples","lsfg27_budget_retry_s","lsfg33_slow_guard"};
     const char *envs[]={"DEFJAM_LSFG27_FLOW","DEFJAM_LSFG27_FP16","DEFJAM_LSFG27_GPU_BUDGET","DEFJAM_LSFG30_MULTIPLIER","DEFJAM_LSFG30_REFRESH","DEFJAM_LSFG32_HEIGHT","DEFJAM_LSFG27_BUDGET_GUARD","DEFJAM_LSFG27_WARMUP","DEFJAM_LSFG27_BUDGET_RETRY","DEFJAM_LSFG33_SLOW_GUARD"};const int defaults[]={25,0,4000,2,60,480,1,60,45,1};
     for(unsigned j=0;j<sizeof(keys)/sizeof(keys[0]);j++){snprintf(value,sizeof value,"%d",recomp_settings_get("graphics",keys[j],defaults[j]));setenv(envs[j],value,1);}
     fprintf(stderr,"[LSFG38-HANDOFF] multiplier=%s output_hz=%s; private snapshots use the LSFG height limit\n",getenv("DEFJAM_LSFG30_MULTIPLIER"),getenv("DEFJAM_LSFG30_REFRESH"));}

    defjam_power25_init();
    int cap=recomp_settings_get("vksched","present_limit",-1);
    const char *launch=getenv("DEFJAM_FPS_LIMIT");
    if(cap<0&&launch&&strcmp(launch,"-1"))cap=atoi(launch);
    if(cap==45)cap=30;
    if(cap==120)cap=60; /* Removed120 experiment: honor legacy config safely. */
    atomic_store(&selected,valid_limit(cap)?cap:-1);
    cap=recomp_settings_get("vksched","menus_limit",60);
    if(cap==45)cap=30;
    atomic_store(&menu_limit,valid_limit(cap)?cap:60);
    atomic_store(&low_end,profile==1||(profile==0&&cpu_low_end()));
    vertex_setting=recomp_settings_get("vksched","vertex_participants",4);
    extern void android_native120_configure140(void);
    android_native120_configure140();
    {int fx=recomp_settings_get("graphics","fxaa145",-1);
     if(fx<0){const char *legacy=getenv("DEFJAM_LEGACY_FXAA145");fx=legacy&&legacy[0]=='1';}
     if(!getenv("DEFJAM_EXPLICIT_FXAA145"))setenv("RECOMP_VK_FXAA",fx?"1":"0",1);}
    defjam_thermal138_init(0); /* policy removed, including legacy INI=1 */
    int ms=recomp_settings_get("vksched","audio_buffer_ms",60);
    char text[16];snprintf(text,sizeof text,"%d",ms);setenv("DEFJAM_AUDIO_BUFFER_MS",text,1);
    setenv("DEFJAM_PATH_CACHE",recomp_settings_get("vksched","path_cache",1)?"1":"0",1);
    defjam_trace_enable(recomp_settings_get("vksched","diagnostics",1)&&recomp_settings_get("vksched","hitch_details",1));
    defjam_monitor_init(recomp_settings_get("vksched","diagnostics",1));
    defjam_guest_sampler_init(recomp_settings_get("vksched","guest_samples",0));
    fprintf(stderr,"[VKSCHED121] profile=%d low_end=%d present_setting=%d menus=%d audio_buffer_ms=%d; simulation unchanged\n",
        profile,atomic_load(&low_end),atomic_load(&selected),atomic_load(&menu_limit),ms);
}
void defjam_sched_gpu(const char *name) {
    if(profile==0&&adreno6(name))atomic_store(&low_end,1);
    fprintf(stderr,"[VKSCHED121] GPU=%s low_end=%d combat_cap=%d menu_cap=%d vertex=%u\n",name,
        atomic_load(&low_end),defjam_sched_limit(1),defjam_sched_limit(0),defjam_sched_vertex_participants(2));
}
int defjam_sched_low_end(void){return atomic_load(&low_end);}
void defjam_sched_observe_steps(double hz){
    if(atomic_load(&auto30_blocked))return;
    if(hz>=55){slow_step_windows=0;return;}
    if(++slow_step_windows>=2){
        atomic_store(&auto30_blocked,1);
        fprintf(stderr,"[VKSCHED121] low cap suspended: fight-step rate %.2fHz in two combat windows; preserve simulation cadence. This session has NOT met the30fps target.\n",hz);
    }
}
/* Only observed host-event waits are subtracted for the fallback heuristic.
 * Raw SIM121/FRAME121 cadence stays unmodified; this is NOT simulated FPS. */
void defjam_sched_observe_timing(unsigned steps,uint64_t wall,uint64_t network_wait){
    if(!wall)return;
    uint64_t active=network_wait<wall?wall-network_wait:0;
    if(network_wait>wall/20)fprintf(stderr,"[VKSCHED128] observed_hz=%.2f net_event_wait_ms=%.2f nonwait_hz_estimate=%.2f; raw simulation counter unchanged\n",
        steps*1e9/wall,network_wait/1e6,active?steps*1e9/active:0.0);
    if(active<wall/10){slow_step_windows=0;return;} /* too little local evidence */
    defjam_sched_observe_steps(steps*1e9/active);
}
void defjam_sched_set_limit145(int cap){fprintf(stderr,"[CONFIG22] live_present_limit=%d\n",cap);if(cap==-1||cap==0||cap==30||cap==60)atomic_store(&selected,cap);}
int defjam_sched_apply_limit138(int cap,int combat){return defjam_power25_limit(cap,combat&&!atomic_load(&auto30_blocked));}
int defjam_sched_limit(int combat) {
    if(!combat){
        slow_step_windows=0;
        if(atomic_exchange(&auto30_blocked,0))fprintf(stderr,"[VKSCHED128] left combat: restore configured presentation policy; fallback belongs only to that fight\n");
    }
    int cap=atomic_load(&selected);
    int blocked=atomic_load(&auto30_blocked);
    if(cap>=0)cap=cap>0&&cap<60&&blocked?60:cap;
    else cap=combat&&atomic_load(&low_end)&&!blocked?30:atomic_load(&menu_limit);
    return defjam_power25_limit(cap,combat&&!atomic_load(&auto30_blocked)); /* Original simulation cadence guard retained. */
}
unsigned defjam_sched_vertex_participants(unsigned requested) {
    if(vertex_setting>=1&&vertex_setting<=8)return (unsigned)vertex_setting;
    (void)requested; return 4; /* one dispatcher plus up to three helpers; explicit 1 restores serial. */
}
#endif

#if defined(__ANDROID__)
int defjam_sched_vertex_cache141(void){return recomp_settings_get("vksched","vertex_cache",1)!=0;}
unsigned defjam_sched_vertex_threshold(void){
    int n=recomp_settings_get("vksched","vertex_min_batch",512);
    return n==96||n==128||n==256||n==512||n==1024||n==2048?(unsigned)n:512u;
}
#endif

#if defined(__ANDROID__)
#include "android_power25_impl.h"
#endif
