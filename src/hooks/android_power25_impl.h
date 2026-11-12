/* Included once by android_sched.c; dynamic APIs keep older Android loadable.
 * The monitor owns sampling/policy; the render owner alone owns ADPF sessions. */
#include "platform/android_power25.h"
#include "power_policy25.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdbool.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <inttypes.h>
#include <math.h>
#include <time.h>
#define P25_THREADS 16
static _Atomic int p25_ready,p25_on,p25_adpf,p25_log,p25_dynamic,p25_allow40,p25_sustained,p25_display,p25_eco_mode,p25_stage,p25_scaled,p25_pause,p25_cap=60;
static _Atomic int p25_tid[P25_THREADS];static char p25_role[P25_THREADS][24];
static _Atomic uint64_t p25_frame_count,p25_frame_wall,p25_work_total,p25_last_work,p25_waits[3];
static _Thread_local uint64_t p25_work_start;static _Thread_local unsigned p25_local_pause_gen;static _Atomic unsigned p25_pause_gen,p25_reset_frames;
static _Atomic int p25_display_req,p25_display_milli,p25_display_mode,p25_display_sustained;
void defjam_power25_display_observed(int requested,int milli_hz,int mode,int sustained){atomic_store(&p25_display_req,requested);atomic_store(&p25_display_milli,milli_hz);atomic_store(&p25_display_mode,mode);atomic_store(&p25_display_sustained,sustained);}
static _Atomic int p25_fg30_hot;static uint64_t p25_fg30_cool;
int defjam_power25_fg30_blocked(void){return atomic_load(&p25_fg30_hot);}
static float p25_head=NAN;static PowerPolicy25 p25_policy;
static uint64_t p25_last_present,p25_report_begin;static unsigned p25_report_frames;
static uint64_t p25_freq_sum,p25_freq_count;static pthread_mutex_t p25_freq_guard=PTHREAD_MUTEX_INITIALIZER;static _Atomic long p25_mean_khz=-1;
static _Atomic int p25_cpu_milli[P25_THREADS],p25_cpu_valid[P25_THREADS],p25_cpu_last[P25_THREADS];
static uint64_t p25_now(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static int p25_text(const char *p,char *b,size_t cap){int f=open(p,O_RDONLY|O_CLOEXEC);if(f<0)return 0;ssize_t n=read(f,b,cap-1);close(f);if(n<=0)return 0;b[n]=0;return 1;}
static long p25_number(const char *p){char b[96];return p25_text(p,b,sizeof b)?strtol(b,NULL,10):-1;}
/* Opaque NDK handles. Function signatures follow android/performance_hint.h
 * and android/thermal.h; unavailable symbols leave features inactive. */
typedef struct APerformanceHintManager P25Manager;
typedef struct APerformanceHintSession P25Session;
typedef struct AThermalManager P25Thermal;
static void *p25_lib;
static P25Manager *(*p25_get_manager)(void);
static P25Session *(*p25_create)(P25Manager *,const int32_t *,size_t,int64_t);
static int (*p25_report)(P25Session *,int64_t);
static int (*p25_target)(P25Session *,int64_t);
static int (*p25_efficient)(P25Session *,bool);
static int64_t (*p25_rate)(P25Manager *);
static void (*p25_close)(P25Session *);
static P25Thermal *(*p25_thermal_acquire)(void);
static float (*p25_headroom)(P25Thermal *,int);
static int (*p25_thermal_status)(P25Thermal *);
static P25Thermal *p25_thermal;
void defjam_power25_init(void){
    int on=recomp_settings_get("vksched","power25",1);
    atomic_store(&p25_on,on);atomic_store(&p25_adpf,on&&recomp_settings_get("vksched","adpf25",1));
    atomic_store(&p25_log,on&&recomp_settings_get("vksched","power_log25",1));
    atomic_store(&p25_dynamic,on&&recomp_settings_get("vksched","dynamic_resolution25",0));
    atomic_store(&p25_allow40,on&&recomp_settings_get("vksched","thermal_cap40_25",0));
    atomic_store(&p25_sustained,on&&recomp_settings_get("vksched","sustained25",1));
    atomic_store(&p25_display,on&&recomp_settings_get("vksched","display_rate25",1));
    atomic_store(&p25_eco_mode,on&&recomp_settings_get("vksched","eco_resolution25",0));
    p25_lib=on?dlopen("libandroid.so",RTLD_NOW|RTLD_LOCAL):NULL;
#define P25_LOAD(name,symbol) do{*(void **)(&name)=p25_lib?dlsym(p25_lib,symbol):NULL;}while(0)
    P25_LOAD(p25_get_manager,"APerformanceHint_getManager");P25_LOAD(p25_create,"APerformanceHint_createSession");
    P25_LOAD(p25_report,"APerformanceHint_reportActualWorkDuration");P25_LOAD(p25_target,"APerformanceHint_updateTargetWorkDuration");
    P25_LOAD(p25_efficient,"APerformanceHint_setPreferPowerEfficiency");P25_LOAD(p25_rate,"APerformanceHint_getPreferredUpdateRateNanos");
    P25_LOAD(p25_close,"APerformanceHint_closeSession");P25_LOAD(p25_thermal_acquire,"AThermal_acquireManager");
    P25_LOAD(p25_headroom,"AThermal_getThermalHeadroom");P25_LOAD(p25_thermal_status,"AThermal_getCurrentThermalStatus");
#undef P25_LOAD
    if(p25_thermal_acquire)p25_thermal=p25_thermal_acquire();
    fprintf(stderr,"[POWER25] enabled=%d ADPF_symbols=%d headroom_api=%d eco480=%d dynamic_resolution=%d cap40_optin=%d; OS controls clocks, original guest clock retained; render_pipeline=unimplemented\n",on,p25_get_manager&&p25_create&&p25_report&&p25_target&&p25_close,p25_thermal&&p25_headroom,atomic_load(&p25_eco_mode),atomic_load(&p25_dynamic),atomic_load(&p25_allow40));
    if(recomp_settings_get("vksched","render_pipeline",0))fprintf(stderr,"[POWER25] render_pipeline=1 unavailable: immutable resource/state snapshots required; existing serial-owner renderer retained\n");
    atomic_store_explicit(&p25_ready,1,memory_order_release);
}
void defjam_power25_register(const char *role){
    int tid=(int)syscall(SYS_gettid);
    for(unsigned i=0;i<P25_THREADS;i++)if(atomic_load(&p25_tid[i])==tid)return;
    int fixed=!strcmp(role,"guest-main")?0:!strcmp(role,"render")?1:!strcmp(role,"audio-mix")?2:!strcmp(role,"guest-irq")?3:-1;
    unsigned first=fixed>=0?(unsigned)fixed:4,last=fixed>=0?(unsigned)fixed+1:P25_THREADS;
    for(unsigned i=first;i<last;i++){int empty=0;if(atomic_compare_exchange_strong(&p25_tid[i],&empty,-1)){
        snprintf(p25_role[i],sizeof p25_role[i],"%s",role);atomic_store_explicit(&p25_tid[i],tid,memory_order_release);return;
    }}
}
void defjam_power25_paused(int p){if(atomic_exchange(&p25_pause,!!p)!=!!p){atomic_fetch_add(&p25_pause_gen,1);atomic_store(&p25_reset_frames,1);}}
void defjam_power25_wait(int kind){if(kind>=0&&kind<3&&atomic_load(&p25_on))atomic_fetch_add_explicit(&p25_waits[kind],1,memory_order_relaxed);}
int defjam_power25_limit(int cap,int combat){
    int result=cap;
    if(combat&&cap==60&&atomic_load(&p25_stage)>=2&&atomic_load(&p25_allow40))result=40;
    atomic_store(&p25_cap,result);return result;
}
int defjam_power25_resolution(void){return atomic_load(&p25_ready)&&atomic_load(&p25_on)&&atomic_load(&p25_dynamic)&&atomic_load(&p25_stage)>0;}
void defjam_power25_resolution_applied(int scaled){atomic_store(&p25_scaled,!!scaled);}
int defjam_power25_eco(void){return atomic_load(&p25_eco_mode);}
int defjam_power25_ui(void){
    if(!atomic_load_explicit(&p25_ready,memory_order_acquire))return 0;
    return (1<<20)|(atomic_load(&p25_sustained)?1:0)|(atomic_load(&p25_display)?2:0)|((atomic_load(&p25_cap)&255)<<8);
}
void defjam_power25_work_begin(void){unsigned gen=atomic_load(&p25_pause_gen);if(gen!=p25_local_pause_gen){p25_work_start=0;p25_local_pause_gen=gen;}if(atomic_load(&p25_on)&&!p25_work_start)p25_work_start=p25_now();}
void defjam_power25_work_end(int cap){
    if(!atomic_load_explicit(&p25_ready,memory_order_acquire)||!atomic_load(&p25_on)||!p25_work_start)return;
    if(p25_local_pause_gen!=atomic_load(&p25_pause_gen)){p25_work_start=0;return;}
    uint64_t now=p25_now(),ns=now>=p25_work_start?now-p25_work_start:0;p25_work_start=0;
    atomic_store(&p25_last_work,ns);atomic_fetch_add(&p25_work_total,ns);
    if(!ns||atomic_load(&p25_pause)||!atomic_load(&p25_adpf))return;
    static P25Session *session;static int32_t members[2];static int64_t target,last_target,rate;static uint64_t last_report;static int failure;
    int32_t ids[2]={(int32_t)syscall(SYS_gettid),0};
    for(unsigned i=0;i<P25_THREADS;i++)if(atomic_load_explicit(&p25_tid[i],memory_order_acquire)>0&&!strcmp(p25_role[i],"guest-main")){ids[1]=atomic_load(&p25_tid[i]);break;}
    if(!p25_get_manager||!p25_create||!p25_report||!p25_target||!p25_close||!ids[1]||failure)return;
    if(session&&(members[0]!=ids[0]||members[1]!=ids[1])){p25_close(session);session=NULL;}
    if(!session){
        P25Manager *m=p25_get_manager();if(!m){failure=1;fprintf(stderr,"[ADPF25] manager unavailable\n");return;}
        target=1000000000ll/(cap>=30&&cap<=165?cap:60);
        session=p25_create(m,ids,2,target);if(!session){failure=1;fprintf(stderr,"[ADPF25] session unsupported/rejected; no scheduling override applied\n");return;}
        memcpy(members,ids,sizeof members);last_target=target;rate=p25_rate?p25_rate(m):0;
        int efficient=p25_efficient?p25_efficient(session,true):-1;
        fprintf(stderr,"[ADPF25] session render_tid=%d guest_tid=%d target_ns=%"PRId64" prefer_power_efficiency_result=%d; combined render work measured before explicit limiter, no fabricated A/B threads\n",ids[0],ids[1],target,efficient);
    }
    target=1000000000ll/(cap>=30&&cap<=165?cap:60);
    if(target!=last_target){if(!p25_target(session,target))last_target=target;}
    if(!rate||!last_report||now-last_report>=(uint64_t)(rate>0?rate:0)){
        int rc=p25_report(session,(int64_t)ns);last_report=now;
        if(rc){fprintf(stderr,"[ADPF25] report failed=%d; session closed\n",rc);p25_close(session);session=NULL;failure=1;}
    }
}
void defjam_power25_present(uint64_t now,int combat,int cap){
    if(!atomic_load(&p25_on))return;
    static uint32_t hist25[4001];static uint64_t hist_n25,over34_25;
    if(atomic_exchange(&p25_reset_frames,0)){p25_last_present=0;p25_report_begin=0;p25_report_frames=0;memset(hist25,0,sizeof hist25);hist_n25=over34_25=0;atomic_store(&p25_work_total,0);}
    if(p25_last_present&&now>=p25_last_present){
        uint64_t dt=now-p25_last_present;atomic_fetch_add(&p25_frame_wall,dt);
        unsigned bin=dt>=400000000ull?4000:(unsigned)((dt+99999ull)/100000ull);hist25[bin]++;hist_n25++;over34_25+=dt>34000000ull;
    }
    p25_last_present=now;atomic_fetch_add(&p25_frame_count,1);
    if(!p25_report_begin)p25_report_begin=now;
    if(++p25_report_frames==240){
        pthread_mutex_lock(&p25_freq_guard);uint64_t freq_sum=p25_freq_sum,freq_n=p25_freq_count;p25_freq_sum=p25_freq_count=0;pthread_mutex_unlock(&p25_freq_guard);
        long mean=freq_n?(long)(freq_sum/freq_n):atomic_load(&p25_mean_khz);
        double seconds=now>p25_report_begin?(now-p25_report_begin)/1e9:0;
        uint64_t seen=0,rank=(hist_n25*95+99)/100;unsigned q=0;for(q=0;q<4000;q++){seen+=hist25[q];if(seen>=rank)break;}
        fprintf(stderr,"[PERF25] 240_frames fps=%.3f p95_upper_ms=%.1f p95_overflow=%d over34=%"PRIu64" combined_render_work_ms_frame=%.3f mean_big_current_khz=%ld cap=%d combat=%d pipeline=0; work excludes explicit limiter; per-thread CPU and core in CPU25; no guarantee of scanout FPS\n",seconds?239/seconds:0,q/10.0,q==4000,over34_25,atomic_exchange(&p25_work_total,0)/1e6/240,mean,cap,combat);
        memset(hist25,0,sizeof hist25);hist_n25=over34_25=0;
        for(unsigned i=0;i<P25_THREADS;i++){int tid=atomic_load_explicit(&p25_tid[i],memory_order_acquire);if(tid>0)fprintf(stderr,"[PERF25-TID] tid=%d role=%s sampled_cpu_pct=%.3f valid=%d sampled_last_cpu=%d; latest1Hz sample, not per-frame CPU measurement\n",tid,p25_role[i],atomic_load(&p25_cpu_milli[i])/1000.0,atomic_load(&p25_cpu_valid[i]),atomic_load(&p25_cpu_last[i]));}
        p25_report_frames=0;p25_report_begin=0;
    }
}
/* Readable fields only; -1/valid=0 mean unavailable, never inferred zero load. */
typedef struct {uint64_t user,sys,birth,delay,ivcsw;int cpu;unsigned valid;} P25Sample;
static P25Sample p25_sample(int tid){
    P25Sample s={.cpu=-1};char p[160],b[8192];
    snprintf(p,sizeof p,"/proc/self/task/%d/stat",tid);
    if(p25_text(p,b,sizeof b)){
        char *at=strrchr(b,')');if(at){at++;unsigned f=3;while(*at&&f<=39){while(*at==' ')at++;if(!*at)break;char *end=at;while(*end&&*end!=' ')end++;
            uint64_t v=strtoull(at,NULL,10);if(f==14)s.user=v;if(f==15)s.sys=v;if(f==22)s.birth=v;if(f==39)s.cpu=(int)v;at=end;f++;
        }if(f==40)s.valid|=1;}
    }
    snprintf(p,sizeof p,"/proc/self/task/%d/schedstat",tid);if(p25_text(p,b,sizeof b)){
        unsigned long long r,d,n;if(sscanf(b,"%llu %llu %llu",&r,&d,&n)==3){s.delay=d;s.valid|=2;}}
    snprintf(p,sizeof p,"/proc/self/task/%d/status",tid);if(p25_text(p,b,sizeof b)){
        char *a=strstr(b,"\nnonvoluntary_ctxt_switches:");if(a){s.ivcsw=strtoull(strchr(a,':')+1,NULL,10);s.valid|=4;}}
    return s;
}
int defjam_power25_monitor_on(void){return atomic_load_explicit(&p25_ready,memory_order_acquire)&&atomic_load(&p25_on);}
void defjam_power25_tick(void){
    if(!atomic_load_explicit(&p25_ready,memory_order_acquire)||!atomic_load(&p25_on))return;
    static uint64_t last;uint64_t now=p25_now();if(last&&now-last<950000000ull)return;
    double seconds=last&&now>last?(now-last)/1e9:0;last=now;
    if(p25_thermal&&p25_headroom&&!atomic_load(&p25_pause))p25_head=p25_headroom(p25_thermal,10);else p25_head=NAN;
    int status=p25_thermal&&p25_thermal_status?p25_thermal_status(p25_thermal):-1;
    /* FG is shed independently of resolution/cap options; no new thermal API poll. */
    if(isfinite(p25_head)&&p25_head>=.85f){
        if(!atomic_exchange(&p25_fg30_hot,1))fprintf(stderr,"[FG30-THERMAL] disabled headroom=%.3f\n",p25_head);
        p25_fg30_cool=0;
    }else if(atomic_load(&p25_fg30_hot)){
        if(isfinite(p25_head)&&p25_head>=0.f&&p25_head<.70f){if(!p25_fg30_cool)p25_fg30_cool=now;if(now-p25_fg30_cool>=30000000000ull){atomic_store(&p25_fg30_hot,0);p25_fg30_cool=0;fprintf(stderr,"[FG30-THERMAL] recovered after30s margin\n");}}
        else p25_fg30_cool=0;
    }
    if((atomic_load(&p25_dynamic)||atomic_load(&p25_allow40))&&!atomic_load(&p25_pause)&&power_policy25_step(&p25_policy,p25_head,now,atomic_load(&p25_scaled),atomic_load(&p25_allow40))){
        atomic_store(&p25_stage,p25_policy.level);
        fprintf(stderr,"[ACTION25] mono_ms=%"PRIu64" headroom_raw=%.3f level=%d action=%s; no OS clock write\n",now/1000000,p25_head,p25_policy.level,p25_policy.level==2?"request40_after_scaled":p25_policy.level==1?"request480":"restore_manual_resolution");
    }
    long currents[4],policies[4];uint64_t sum_freq=0,n_freq=0;
    for(int c=4;c<=7;c++){
        char p[192];snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",c);currents[c-4]=p25_number(p);
        snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",c);policies[c-4]=p25_number(p);
        if(currents[c-4]>0){sum_freq+=(uint64_t)currents[c-4];n_freq++;}
    }
    pthread_mutex_lock(&p25_freq_guard);p25_freq_sum+=sum_freq;p25_freq_count+=n_freq;if(p25_freq_count)atomic_store(&p25_mean_khz,(long)(p25_freq_sum/p25_freq_count));pthread_mutex_unlock(&p25_freq_guard);
    if(!atomic_load(&p25_log))return;
    fprintf(stderr,"[POWER25-1S] mono_ms=%"PRIu64" paused=%d headroom_raw=%.3f thermal_status=%d level=%d scaled=%d cap=%d current_khz=%ld/%ld/%ld/%ld policy_max_khz=%ld/%ld/%ld/%ld; shared cluster samples, headroom1=SEVERE\n",now/1000000,atomic_load(&p25_pause),p25_head,status,atomic_load(&p25_stage),atomic_load(&p25_scaled),atomic_load(&p25_cap),currents[0],currents[1],currents[2],currents[3],policies[0],policies[1],policies[2],policies[3]);
    fprintf(stderr,"[DISPLAY25-1S] requested_hz=%d observed_hz=%.3f observed_mode=%d sustained_applied=%d; Android may override requests\n",atomic_load(&p25_display_req),atomic_load(&p25_display_milli)/1000.0,atomic_load(&p25_display_mode),atomic_load(&p25_display_sustained));
    static P25Sample prev[P25_THREADS];static int old_tid[P25_THREADS];long hz=sysconf(_SC_CLK_TCK);
    for(unsigned i=0;i<P25_THREADS;i++){
        int tid=atomic_load_explicit(&p25_tid[i],memory_order_acquire);if(tid<=0)continue;
        P25Sample a=p25_sample(tid),b=prev[i];int valid=(a.valid&1)&&(b.valid&1)&&tid==old_tid[i]&&a.birth==b.birth&&a.user>=b.user&&a.sys>=b.sys&&seconds>0&&hz>0;
        int delay=(a.valid&b.valid&2)&&a.delay>=b.delay,ctx=(a.valid&b.valid&4)&&a.ivcsw>=b.ivcsw;
        fprintf(stderr,"[CPU25-1S] mono_ms=%"PRIu64" tid=%d role=%s valid=%d last_cpu=%d cpu_pct=%.2f system_pct=%.2f run_delay_ms=%.3f involuntary_delta=%lld; futex_syscalls_s=unavailable ioctl_syscalls_s=unavailable\n",now/1000000,tid,p25_role[i],valid,a.cpu,valid?(a.user-b.user+a.sys-b.sys)*100.0/hz/seconds:-1,valid?(a.sys-b.sys)*100.0/hz/seconds:-1,valid&&delay?(a.delay-b.delay)/1e6:-1,valid&&ctx?(long long)(a.ivcsw-b.ivcsw):-1);
        atomic_store(&p25_cpu_milli[i],valid?(int)((a.user-b.user+a.sys-b.sys)*100000.0/hz/seconds):-1000);atomic_store(&p25_cpu_valid[i],valid);atomic_store(&p25_cpu_last[i],a.cpu);
        prev[i]=a;old_tid[i]=tid;
    }
    fprintf(stderr,"[SYNC25-1S] worker_cond_wait_calls=%"PRIu64" dispatcher_cond_wait_calls=%"PRIu64" helper_signals=%"PRIu64" interval_s=%.3f; application calls, NOT measured futex syscalls\n",atomic_exchange(&p25_waits[0],0),atomic_exchange(&p25_waits[1],0),atomic_exchange(&p25_waits[2],0),seconds);
    DIR *dir=opendir("/sys/class/thermal");struct dirent *e;if(dir){while((e=readdir(dir))){
        int zone=!strncmp(e->d_name,"thermal_zone",12),cool=!strncmp(e->d_name,"cooling_device",14);if(!zone&&!cool)continue;
        char p[384],type[96];snprintf(p,sizeof p,"/sys/class/thermal/%s/type",e->d_name);if(!p25_text(p,type,sizeof type))continue;type[strcspn(type,"\r\n")]=0;
        if(zone&&!strstr(type,"cpu")&&!strstr(type,"gpu")&&!strstr(type,"battery")&&!strstr(type,"skin"))continue;
        snprintf(p,sizeof p,"/sys/class/thermal/%s/%s",e->d_name,zone?"temp":"cur_state");long v=p25_number(p);if(cool&&v<=0)continue;
        fprintf(stderr,"[THERM25-1S] mono_ms=%"PRIu64" node=%s type=%s %s=%ld\n",now/1000000,e->d_name,type,zone?"temp_raw":"cooling_state",v);
    }closedir(dir);}
}
