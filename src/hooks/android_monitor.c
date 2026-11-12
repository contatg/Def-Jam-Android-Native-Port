/* App present-completion intervals, not physical GPU/display timing. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "platform/android_sched.h"
#include "platform/android_thread_policy.h"
#include "platform/android_hitch.h"
#include "platform/android_power25.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>
#include <math.h>
#define HIST 8001
#define WINDOWS 32
typedef struct Window { uint64_t sequence,n,sum,present; double seconds,p50,p95,p99; int combat,cap; unsigned steps; } Window;
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER,worker_guard=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int started,enabled,stop;
static _Atomic int paused,thermal=-1,reset;
static _Atomic unsigned sim_steps;
static _Atomic uint64_t network_wait128;
static uint64_t network_begin128;
uint64_t defjam_monitor_network_wait_total(void){return atomic_load_explicit(&network_wait128,memory_order_relaxed);}
void defjam_monitor_network_wait(uint64_t ns){atomic_fetch_add_explicit(&network_wait128,ns,memory_order_relaxed);}
void defjam_monitor_fight_step(void){atomic_fetch_add_explicit(&sim_steps,1,memory_order_relaxed);}
static _Atomic unsigned audio_queued,audio_gaps,audio_refused,audio_missing;
static float hud[6];
static uint64_t epoch,last,last_hud,n,sum,present_sum,seq,report_started;
static unsigned hist[HIST],hist10[HIST];
static uint64_t n10,sum10,over10,ten_epoch;
static int ten_phase=-1,ten_cap=-1;
static double percentile10(unsigned p){
    uint64_t rank=(n10*p+99)/100,seen=0;if(!n10)return 0;
    for(unsigned i=0;i<HIST;i++){seen+=hist10[i];if(seen>=rank)return i==HIST-1?INFINITY:i*.1;}
    return INFINITY;
}
static void reset10(uint64_t now,int combat,int cap){memset(hist10,0,sizeof hist10);n10=sum10=over10=0;ten_epoch=now;ten_phase=combat;ten_cap=cap;}

static int phase=-1,target=-1;
static unsigned sim_begin;
static Window windows[WINDOWS];static unsigned queued,overflow;
static uint64_t mono(void){struct timespec t;return clock_gettime(CLOCK_MONOTONIC,&t)?0:(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
static double percentile(unsigned p) {
    uint64_t rank=(n*p+99)/100,seen=0;if(!n)return 0;
    for(unsigned i=0;i<HIST;i++){seen+=hist[i];if(seen>=rank)return i==HIST-1?INFINITY:i*.1;}
    return INFINITY;
}
static void new_window(uint64_t now,int combat,int cap) {
    epoch=now;n=sum=present_sum=0;memset(hist,0,sizeof hist);phase=combat;target=cap;sim_begin=atomic_load(&sim_steps);network_begin128=atomic_load(&network_wait128);
}
void defjam_monitor_frame(uint64_t now,int combat,int cap,uint64_t present_ns) {
    pthread_mutex_lock(&guard);
    if(atomic_load(&paused)){last=0;pthread_mutex_unlock(&guard);return;}
    if(atomic_exchange(&reset,0)||!last||now<=last||phase!=combat||target!=cap){
        new_window(now,combat,cap);reset10(now,combat,cap);defjam_trace_frame(now,0,combat);last=now;last_hud=now;pthread_mutex_unlock(&guard);return;
    }
    uint64_t dt=now-last;last=now;n++;sum+=dt;present_sum+=present_ns;
    unsigned bin=dt>=800000000ull?HIST-1:(unsigned)((dt+99999ull)/100000ull);hist[bin]++;
    hist10[bin]++;n10++;sum10+=dt;over10+=dt>34000000ull;
    defjam_trace_frame(now,dt,combat);
    if(now-last_hud>=500000000ull){
        hud[0]=sum?(float)((double)n*1e9/sum):0;hud[1]=n?(float)(sum/1e6/n):0;
        hud[2]=(float)percentile(95);hud[3]=(float)percentile(99);hud[4]=(float)cap;
        hud[5]=n?(float)(present_sum/1e6/n):0;last_hud=now;
    }
    if(now-epoch>=2000000000ull){
        if(combat&&cap>0&&cap<60)defjam_sched_observe_timing((unsigned)(atomic_load(&sim_steps)-sim_begin),now-epoch,atomic_load(&network_wait128)-network_begin128);
        if(enabled){
            Window w={++seq,n,sum,present_sum,(now-epoch)/1e9,percentile(50),percentile(95),percentile(99),combat,cap,(unsigned)(atomic_load(&sim_steps)-sim_begin)};
            if(queued<WINDOWS)windows[queued++]=w;else overflow++;
        }
        new_window(now,combat,cap);
    }
    pthread_mutex_unlock(&guard);
}
void defjam_monitor_hud(float out[6]){
    pthread_mutex_lock(&guard);uint64_t now=mono();
    if(!last||atomic_load(&paused)||(now>last&&now-last>1000000000ull))memset(out,0,sizeof hud);
    else memcpy(out,hud,sizeof hud);
    pthread_mutex_unlock(&guard);
}
void defjam_monitor_pause(int value){if(atomic_exchange(&paused,!!value)!=!!value)atomic_store(&reset,1);}
void defjam_monitor_thermal(int value){atomic_store(&thermal,value);}
void defjam_monitor_audio(unsigned queued_frames,unsigned gaps,unsigned refused,unsigned missing){
    atomic_store_explicit(&audio_queued,queued_frames,memory_order_relaxed);
    atomic_store_explicit(&audio_gaps,gaps,memory_order_relaxed);
    atomic_store_explicit(&audio_refused,refused,memory_order_relaxed);
    atomic_fetch_add_explicit(&audio_missing,missing,memory_order_relaxed);
}
static long read_number(const char *path){long value=-1;FILE *f=fopen(path,"r");if(f){if(fscanf(f,"%ld",&value)!=1)value=-1;fclose(f);}return value;}
typedef struct ThreadPrev { long tid; uint64_t user,sys; } ThreadPrev;
static ThreadPrev previous[512];static unsigned previous_n;static uint64_t proc_epoch;
static void thread_report(uint64_t now){
    DIR *d=opendir("/proc/self/task");if(!d){fprintf(stderr,"[CPU121] thread counters unavailable\n");return;}
    struct dirent *e;ThreadPrev next[512];unsigned count=0,reported=0;
    double seconds=proc_epoch&&now>proc_epoch?(now-proc_epoch)/1e9:0;long hz=sysconf(_SC_CLK_TCK);
    while((e=readdir(d))&&count<512){
        char *end;long tid=strtol(e->d_name,&end,10);if(*end||tid<=0)continue;
        char path[160],line[4096],name[64]="?",state='?';snprintf(path,sizeof path,"/proc/self/task/%ld/stat",tid);
        FILE *f=fopen(path,"r");if(!f)continue;int ok=fgets(line,sizeof line,f)!=NULL;fclose(f);if(!ok)continue;
        char *open=strchr(line,'('),*close=strrchr(line,')');if(!open||!close)continue;
        size_t length=(size_t)(close-open-1);if(length>=sizeof name)length=sizeof name-1;memcpy(name,open+1,length);name[length]=0;
        uint64_t user=0,sys=0;int cpu=-1;char *save,*token=strtok_r(close+2," ",&save);int field=3;
        while(token){if(field==3)state=*token;if(field==14)user=strtoull(token,NULL,10);if(field==15)sys=strtoull(token,NULL,10);if(field==39)cpu=atoi(token);field++;token=strtok_r(NULL," ",&save);}
        next[count++]=(ThreadPrev){tid,user,sys};
        for(unsigned j=0;seconds&&hz>0&&j<previous_n;j++)if(previous[j].tid==tid){
            if(user>=previous[j].user&&sys>=previous[j].sys){
                double u=100.0*(user-previous[j].user)/hz/seconds,k=100.0*(sys-previous[j].sys)/hz/seconds;
                if(u+k>=.5||!strncmp(name,"dj-vertex",9)||!strcmp(name,"dj-render")||!strncmp(name,"dj-guest",8)||!strcmp(name,"dj-audio")){
                    cpu_set_t set;char mask[512]="unavailable";size_t pos=0;
                    if(!sched_getaffinity((pid_t)tid,sizeof set,&set))for(int c=0;c<CPU_SETSIZE&&pos+12<sizeof mask;c++)if(CPU_ISSET(c,&set))pos+=(size_t)snprintf(mask+pos,sizeof mask-pos,"%s%d",pos?",":"",c);
                    fprintf(stderr,"[CPU121] tid=%ld name=%s state=%c user_pct=%.2f kernel_pct=%.2f last_cpu=%d allowed={%s}\n",tid,name,state,u,k,cpu,mask);reported++;
                }
            }break;
        }
    }
    closedir(d);memcpy(previous,next,count*sizeof next[0]);previous_n=count;proc_epoch=now;
    fprintf(stderr,"[CPU121] tracked=%u reported=%u interval_s=%.3f; last_cpu is not affinity; first/new TIDs need a baseline\n",count,reported,seconds);
}
static void hardware_report(void){
    for(int cpu=0;cpu<64;cpu++){
        char p[192];snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d",cpu);
        if(access(p,F_OK))continue;
        snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",cpu);long current=read_number(p);
        snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",cpu);long policy=read_number(p);
        snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq",cpu);long max=read_number(p);
        fprintf(stderr,"[FREQ121] cpu=%d current_khz=%ld policy_max_khz=%ld hardware_max_khz=%ld\n",cpu,current,policy,max);
    }
    DIR *d=opendir("/sys/class/thermal");struct dirent *e;unsigned zones=0,cooling=0;
    if(d){while((e=readdir(d))){
        int zone=!strncmp(e->d_name,"thermal_zone",12),cool=!strncmp(e->d_name,"cooling_device",14);
        if(!zone&&!cool)continue;
        char p[320],type[96]="?";snprintf(p,sizeof p,"/sys/class/thermal/%s/type",e->d_name);
        FILE *f=fopen(p,"r");if(f){if(!fgets(type,sizeof type,f))strcpy(type,"?");fclose(f);}type[strcspn(type,"\r\n")]=0;
        snprintf(p,sizeof p,"/sys/class/thermal/%s/%s",e->d_name,zone?"temp":"cur_state");long value=read_number(p);
        if(zone&&zones<24&&value>=0){fprintf(stderr,"[THERM121] zone=%s type=%s temp_raw=%ld\n",e->d_name,type,value);zones++;}
        if(cool&&cooling<24&&value>0){fprintf(stderr,"[THERM121] cooling=%s type=%s state=%ld\n",e->d_name,type,value);cooling++;}
    }closedir(d);}
    fprintf(stderr,"[THERM121] android_status=%d readable_zones=%u active_cooling=%u; -1=unavailable; temperature/frequency alone do not prove throttling\n",atomic_load(&thermal),zones,cooling);
}
static void report(void){
    Window copy[WINDOWS];unsigned count,dropped;
    pthread_mutex_lock(&guard);
    uint64_t frames10=n10,total10=sum10,over34=over10;
    double p50_10=percentile10(50),p95_10=percentile10(95),p99_10=percentile10(99);
    int phase10=ten_phase,cap10=ten_cap;double seconds10=last>=ten_epoch?(last-ten_epoch)/1e9:0;
    reset10(last,ten_phase,ten_cap);pthread_mutex_unlock(&guard);
    fprintf(stderr,"[FRAME123-10S] seconds=%.3f mode=%s cap=%d frames=%llu fps=%.3f p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f over34=%llu over34_pct=%.3f; full histogram, context changes restart the interval\n",seconds10,phase10==1?"combat":"menu_or_video",cap10,(unsigned long long)frames10,total10?frames10*1e9/total10:0,p50_10,p95_10,p99_10,(unsigned long long)over34,frames10?100.0*over34/frames10:0);
    pthread_mutex_lock(&guard);count=queued;memcpy(copy,windows,count*sizeof copy[0]);queued=0;dropped=overflow;overflow=0;pthread_mutex_unlock(&guard);
    unsigned combat=0,pass=0;
    for(unsigned i=0;i<count;i++){
        Window *w=&copy[i];int good=w->combat&&w->cap==30&&w->p99<34.0;
        if(w->combat&&w->cap==30){combat++;pass+=good;}
        fprintf(stderr,"[FRAME121] window=%llu seconds=%.3f mode=%s cap=%d frames=%llu fps=%.2f p50_ms=%.2f p95_ms=%.2f p99_ms=%.2f present_ms=%.3f sim_hz=%.2f pass30=%d\n",
            (unsigned long long)w->sequence,w->seconds,w->combat?"combat":"menu_or_video",w->cap,(unsigned long long)w->n,w->sum?(double)w->n*1e9/w->sum:0,w->p50,w->p95,w->p99,w->n?w->present/1e6/w->n:0,w->steps/w->seconds,good);
    }
    fprintf(stderr,"[FRAME121-SUM] combat30_windows=%u p99_below34=%u pct=%.1f missing_windows=%u; host-present intervals, not GPU duration/scanout\n",combat,pass,combat?100.0*pass/combat:0,dropped);
    pthread_mutex_lock(&guard);uint64_t latest=last;pthread_mutex_unlock(&guard);
    uint64_t check=mono();if(!latest||check-latest>1000000000ull)
        fprintf(stderr,"[FRAME121-IDLE] paused=%d since_present_s=%.3f\n",atomic_load(&paused),latest?(check-latest)/1e9:-1.0);
    extern void defjam_make_space_stats(unsigned *,unsigned *);
    unsigned slept,yielded;defjam_make_space_stats(&slept,&yielded);
    fprintf(stderr,"[SPACE122] sleep50us_calls=%u yields=%u; original MakeSpace predicates/fences retained\n",slept,yielded);
    static unsigned old_gaps,old_refused;
    unsigned gaps=atomic_load(&audio_gaps),refused=atomic_load(&audio_refused);
    fprintf(stderr,"[AUDIO121] gaps_delta=%u refused_delta=%u missing_samples=%u queued_ms=%.2f\n",gaps-old_gaps,refused-old_refused,atomic_exchange(&audio_missing,0),atomic_load(&audio_queued)/48.0);old_gaps=gaps;old_refused=refused;
    static unsigned old_steps;
    uint64_t now=mono();unsigned current_steps=atomic_load(&sim_steps),steps=current_steps-old_steps;old_steps=current_steps;
    fprintf(stderr,"[SIM121] fight_steps=%u interval_s=%.3f steps_per_wall_s=%.2f; check during continuous combat, do not infer simulation rate from FPS\n",steps,report_started?(now-report_started)/1e9:0,report_started?steps/((now-report_started)/1e9):0);report_started=now;
    unsigned path_hits,path_misses,path_cases;
    extern void xbox_android_path_stats(unsigned *,unsigned *,unsigned *);
    xbox_android_path_stats(&path_hits,&path_misses,&path_cases);
    fprintf(stderr,"[PATH123] cache_hits=%u new_missing=%u case_corrections=%u; read-only disc paths only, saves uncached\n",path_hits,path_misses,path_cases);
    defjam_trace_report();
    thread_report(now);hardware_report();defjam_perf138_report();defjam_thermal138_report();defjam_guest_sampler_report();fflush(stderr);
}
static void *run(void *unused){
    (void)unused;defjam_android_native_thread_enter("observe","monitor",(uintptr_t)run);report_started=mono();thread_report(report_started);
    pthread_mutex_lock(&worker_guard);
    while(!stop){
        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_sec+=1;
        int result=0;while(!stop&&result==0)result=pthread_cond_timedwait(&wake,&worker_guard,&until);
        if(stop)break;
        pthread_mutex_unlock(&worker_guard);
        defjam_power25_tick();
        defjam_trace_poll22();
        if(mono()-report_started>=10000000000ull)report();
        pthread_mutex_lock(&worker_guard);
    }
    pthread_mutex_unlock(&worker_guard);return NULL;
}
static void finish(void){
    if(!started)return;
    pthread_mutex_lock(&worker_guard);stop=1;pthread_cond_signal(&wake);pthread_mutex_unlock(&worker_guard);pthread_join(worker,NULL);
}
void defjam_monitor_init(int on){
    enabled=on;
    if((on||defjam_power25_monitor_on())&&!started){int result=pthread_create(&worker,NULL,run,NULL);if(!result){started=1;atexit(finish);}else fprintf(stderr,"[FRAME121] monitor unavailable: pthread_create=%d\n",result);}
}
#endif
