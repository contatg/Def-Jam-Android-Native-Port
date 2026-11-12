#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* Scoped wall time, not inferred CPU/GPU utilization. Main, render and mixer
 * lanes overlap: never sum the lanes. Untagged work remains explicitly unknown. */
#if defined(__ANDROID__)
#include "platform/android_hitch.h"
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/resource.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include "recomp_settings.h"
#define LANES123 3
#define HITCHES123 512
typedef struct { _Atomic unsigned sequence,category;_Atomic uint64_t start,total[DJ_CATEGORIES]; } TraceLane;
static TraceLane lanes[LANES123];
static _Atomic int enabled;
static _Thread_local int lane=-1;
static _Atomic int64_t gc_count=-1,gc_ms=-1,java_alloc=-1;
static uint64_t previous[LANES123][DJ_CATEGORIES];
typedef struct {unsigned active_ordinal,argument,caller;uint64_t frame,dt,ns[LANES123][DJ_CATEGORIES];unsigned valid;int combat;} Hitch;
static Hitch queue[HITCHES123];static unsigned queued,dropped;static uint64_t frame_no;
static uint64_t sum[LANES123][DJ_CATEGORIES];
static pthread_mutex_t guard=PTHREAD_MUTEX_INITIALIZER;
static const char *names[DJ_CATEGORIES]={"unscoped_wall","file_io","sync_wait","other_kernel","gather","draw_record","texture_upload","pipeline_create","shader_create","alloc_scoped","audio_mix","pacer","pb_parse22","combiner_lookup22","pipeline_lookup22","uniform22","mapped_copy22","vk_record22","queue_submit_wall22","queue_present_wall22"};
#define ORD125_SLOTS 256
/* Completed outermost main-thread calls. Inclusive wall, not CPU time.
 * Never include nested guest callbacks twice in the ranking. */
typedef struct {unsigned ordinal,argument,caller;uint64_t calls,ns,max;} Ord125;
static Ord125 ord125[ORD125_SLOTS];
static unsigned ord125_overflow;
static _Atomic unsigned ord127_fast_calls;
#define ORD127_MIN_NS 100000ull /* rank blocking/slow calls; avoid a table lock per fast syscall */
static _Atomic unsigned active_sequence125,active_ordinal125,active_argument125,active_caller125;
static _Atomic long main_tid125;
static _Thread_local int outer125;
static _Thread_local unsigned ordinal125,argument125,caller125;
static uint64_t now_ns(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;}
/* v22 diagnostics. No inferred GPU time or invented ioctl timing. */
static _Atomic int fine22,resources22,telemetry22;
static _Atomic long tids22[LANES123];
static _Atomic uint64_t telemetry_time22;
static const char *role22[LANES123]={"guest_main","render","mixer"};
unsigned defjam_trace_fine22(unsigned category){
    return atomic_load_explicit(&fine22,memory_order_relaxed)?defjam_trace_push(category):0;
}
static int text22(const char *path,char *out,size_t cap){
    int fd=open(path,O_RDONLY|O_CLOEXEC);if(fd<0)return 0;
    ssize_t n=read(fd,out,cap-1);close(fd);if(n<=0)return 0;out[n]=0;return 1;
}
static long number22(const char *path){char b[96];return text22(path,b,sizeof b)?strtol(b,NULL,10):-1;}
typedef struct {uint64_t minflt,majflt,utime,stime,delay,ivcsw;unsigned valid;} Resource22;
static Resource22 resource22(long tid){
    Resource22 r={0};char path[128],b[4096];
    if(tid<=0)return r;
    snprintf(path,sizeof path,"/proc/self/task/%ld/stat",tid);
    if(text22(path,b,sizeof b)){
        char *p=strrchr(b,')');if(p){p++;unsigned field=3;
            while(*p && field<=15){while(*p==' ')p++;if(!*p)break;
                char *end=p;while(*end&&*end!=' ')end++;
                uint64_t v=strtoull(p,NULL,10);
                if(field==10)r.minflt=v;if(field==12)r.majflt=v;
                if(field==14)r.utime=v;if(field==15)r.stime=v;
                p=end;field++;
            }if(field==16)r.valid|=1;
        }
    }
    snprintf(path,sizeof path,"/proc/self/task/%ld/schedstat",tid);
    if(text22(path,b,sizeof b)){
        unsigned long long runtime,delay,times;
        if(sscanf(b,"%llu %llu %llu",&runtime,&delay,&times)==3){r.delay=delay;r.valid|=2;}
    }
    snprintf(path,sizeof path,"/proc/self/task/%ld/status",tid);
    if(text22(path,b,sizeof b)){
        char *p=strstr(b,"\nnonvoluntary_ctxt_switches:");
        if(p){r.ivcsw=strtoull(strchr(p,':')+1,NULL,10);r.valid|=4;}
    }return r;
}
static void resources_frame22(uint64_t frame,uint64_t dt){
    if(!atomic_load(&resources22))return;
    static Resource22 prev[LANES123];static long previous_tid[LANES123];
    static uint64_t sums[LANES123][6],valid_frames[LANES123];
    long ticks=sysconf(_SC_CLK_TCK);
    for(unsigned l=0;l<LANES123;l++){
        long tid=atomic_load(&tids22[l]);Resource22 r=resource22(tid),p=prev[l];
        int valid=r.valid==7&&p.valid==7&&previous_tid[l]==tid&&r.minflt>=p.minflt&&r.majflt>=p.majflt&&r.utime>=p.utime&&r.stime>=p.stime&&r.delay>=p.delay&&r.ivcsw>=p.ivcsw;
        if(valid){
            uint64_t delta[6]={r.minflt-p.minflt,r.majflt-p.majflt,r.utime-p.utime,r.stime-p.stime,r.delay-p.delay,r.ivcsw-p.ivcsw};
            for(unsigned i=0;i<6;i++)sums[l][i]+=delta[i];valid_frames[l]++;
            if(dt>50000000ull)fprintf(stderr,"[RESOURCE22-HITCH] frame=%"PRIu64" lane=%s tid=%ld minflt=%"PRIu64" majflt=%"PRIu64" user_ticks=%"PRIu64" sys_ticks=%"PRIu64" run_delay_ms=%.3f involuntary=%"PRIu64" telemetry_age_ms=%.3f\n",frame,role22[l],tid,delta[0],delta[1],delta[2],delta[3],delta[4]/1e6,delta[5],(now_ns()-atomic_load(&telemetry_time22))/1e6);
        }
        if(!valid&&dt>50000000ull)fprintf(stderr,"[RESOURCE22-HITCH] frame=%"PRIu64" lane=%s tid=%ld valid=%u baseline_or_unavailable=1\n",frame,role22[l],tid,r.valid);
        prev[l]=r;previous_tid[l]=tid;
        if(frame%240==0){
            uint64_t n=valid_frames[l];double denom=n?(double)n:1;
            fprintf(stderr,"[RESOURCE22-PERF] frame=%"PRIu64" lane=%s valid_frames=%"PRIu64" minflt_frame=%.3f majflt_frame=%.3f user_ms_frame=%.3f sys_ms_frame=%.3f run_delay_ms_frame=%.3f involuntary_frame=%.3f; /proc deltas, sys_ms includes all kernel CPU (not ioctl wall)\n",frame,role22[l],n,sums[l][0]/denom,sums[l][1]/denom,ticks>0?sums[l][2]*1000.0/ticks/denom:-1,ticks>0?sums[l][3]*1000.0/ticks/denom:-1,sums[l][4]/1e6/denom,sums[l][5]/denom);
            memset(sums[l],0,sizeof sums[l]);valid_frames[l]=0;
        }
    }
    /* Current calling thread only: getrusage cannot sample an arbitrary TID. */
    static struct rusage old;static long oldtid;struct rusage cur;long tid=(long)syscall(SYS_gettid);
    if(!getrusage(RUSAGE_THREAD,&cur)){
        if(oldtid==tid&&(dt>50000000ull||frame%240==0))fprintf(stderr,"[RUSAGE22] frame=%"PRIu64" sampled_tid=%ld minflt=%ld majflt=%ld involuntary=%ld; current-thread delta since previous present sample\n",frame,tid,cur.ru_minflt-old.ru_minflt,cur.ru_majflt-old.ru_majflt,cur.ru_nivcsw-old.ru_nivcsw);
        old=cur;oldtid=tid;
    }
}
void defjam_trace_poll22(void){
    if(!atomic_load(&telemetry22)||!atomic_load(&enabled))return;
    uint64_t now=now_ns();atomic_store(&telemetry_time22,now);
    for(int cpu=4;cpu<=7;cpu++){
        char p[160];snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",cpu);long freq=number22(p);
        snprintf(p,sizeof p,"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",cpu);long max=number22(p);
        fprintf(stderr,"[HW22] mono_ms=%"PRIu64" cpu=%d freq_khz=%ld policy_max_khz=%ld\n",now/1000000,cpu,freq,max);
    }
    DIR *d=opendir("/sys/class/thermal");struct dirent *e;
    if(d){while((e=readdir(d))){
        int zone=!strncmp(e->d_name,"thermal_zone",12),cool=!strncmp(e->d_name,"cooling_device",14);
        if(!zone&&!cool)continue;
        char p[384],type[96];snprintf(p,sizeof p,"/sys/class/thermal/%s/type",e->d_name);
        if(!text22(p,type,sizeof type))continue;type[strcspn(type,"\r\n")]=0;
        snprintf(p,sizeof p,"/sys/class/thermal/%s/%s",e->d_name,zone?"temp":"cur_state");long value=number22(p);
        fprintf(stderr,"[HW22] mono_ms=%"PRIu64" node=%s type=%s %s=%ld\n",now/1000000,e->d_name,type,zone?"temp_raw":"cooling_state",value);
    }closedir(d);}
    for(unsigned l=0;l<LANES123;l++){
        long tid=atomic_load(&tids22[l]);Resource22 r=resource22(tid);
        fprintf(stderr,"[SCHED22] mono_ms=%"PRIu64" lane=%s tid=%ld valid=%u cumulative_run_delay_ns=%"PRIu64"\n",now/1000000,role22[l],tid,r.valid,r.delay);
    }
}

void defjam_trace_enable(int on){
    atomic_store(&fine22,recomp_settings_get("vksched","fine_scopes22",1));
    atomic_store(&resources22,recomp_settings_get("vksched","resources22",0));
    atomic_store(&telemetry22,recomp_settings_get("vksched","telemetry22",1));
    atomic_store(&enabled,!!on);
    fprintf(stderr,"[DIAG22] fine=%d resources=%d telemetry=%d; resources add per-frame proc IO; driver scopes are wall time, ioctl exclusive unavailable\n",atomic_load(&fine22),atomic_load(&resources22),atomic_load(&telemetry22));
}
void defjam_trace_thread(unsigned n){
    if(n>=LANES123)return;lane=(int)n;atomic_store(&tids22[n],(long)syscall(SYS_gettid));
    if(n==0)atomic_store(&main_tid125,(long)syscall(SYS_gettid));
    atomic_store(&lanes[n].start,now_ns());
}
static unsigned change(unsigned category){
    TraceLane *r=&lanes[lane];uint64_t now=now_ns();
    atomic_fetch_add_explicit(&r->sequence,1,memory_order_acq_rel);
    unsigned old=atomic_load_explicit(&r->category,memory_order_relaxed);
    uint64_t start=atomic_load_explicit(&r->start,memory_order_relaxed);
    if(start&&now>=start)atomic_fetch_add_explicit(&r->total[old],now-start,memory_order_relaxed);
    atomic_store_explicit(&r->start,now,memory_order_relaxed);atomic_store_explicit(&r->category,category,memory_order_relaxed);
    atomic_fetch_add_explicit(&r->sequence,1,memory_order_release);return old+1;
}
unsigned defjam_trace_push(unsigned category){if(lane<0||!atomic_load_explicit(&enabled,memory_order_relaxed)||category>=DJ_CATEGORIES)return 0;return change(category);}
void defjam_trace_pop(unsigned token){if(token&&lane>=0&&token<=DJ_CATEGORIES)change(token-1);}
uint64_t defjam_kernel_trace_begin(unsigned ordinal,uint32_t argument,uint32_t caller){
    if(lane!=0||outer125||!atomic_load_explicit(&enabled,memory_order_relaxed))return 0;
    uint64_t start=now_ns();if(!start)return 0;
    outer125=1;ordinal125=ordinal;argument125=argument;caller125=caller;
    atomic_fetch_add(&active_sequence125,1);atomic_store(&active_argument125,argument);atomic_store(&active_caller125,caller);atomic_store(&active_ordinal125,ordinal+1);atomic_fetch_add(&active_sequence125,1);
    return start;
}
void defjam_kernel_trace_end(uint64_t start){
    if(!start)return;uint64_t now=now_ns(),dt=now>=start?now-start:0;
    atomic_fetch_add(&active_sequence125,1);atomic_store(&active_ordinal125,0);atomic_fetch_add(&active_sequence125,1);outer125=0;
    if(dt<ORD127_MIN_NS){atomic_fetch_add_explicit(&ord127_fast_calls,1,memory_order_relaxed);return;}
    unsigned hash=(ordinal125*2654435761u)^argument125^caller125;
    pthread_mutex_lock(&guard);
    for(unsigned i=0;i<ORD125_SLOTS;i++){
        Ord125 *e=&ord125[(hash+i)%ORD125_SLOTS];
        if(!e->calls){e->ordinal=ordinal125;e->argument=argument125;e->caller=caller125;}
        if(e->ordinal==ordinal125&&e->argument==argument125&&e->caller==caller125){e->calls++;e->ns+=dt;if(dt>e->max)e->max=dt;pthread_mutex_unlock(&guard);return;}
    }
    ++ord125_overflow;pthread_mutex_unlock(&guard);
}
static void ordinal_report125(void){
    Ord125 copy[ORD125_SLOTS];unsigned lost;
    pthread_mutex_lock(&guard);memcpy(copy,ord125,sizeof copy);memset(ord125,0,sizeof ord125);lost=ord125_overflow;ord125_overflow=0;pthread_mutex_unlock(&guard);
    for(unsigned rank=0;rank<10;rank++){
        unsigned best=0;for(unsigned i=1;i<ORD125_SLOTS;i++)if(copy[i].ns>copy[best].ns)best=i;
        Ord125 *e=&copy[best];if(!e->calls)break;
        const char *kind=e->ordinal==99?"interval_va":e->ordinal==158||e->ordinal==235?"object_array_va":e->ordinal==159||e->ordinal==277?"guest_object_va":e->ordinal==233||e->ordinal==234?"handle":"arg0";
        fprintf(stderr,"[ORD125] rank=%u linux_tid=%ld ordinal=%u key_kind=%s key=%08X caller=%08X calls=%llu inclusive_wall_ms=%.3f max_call_ms=%.3f; completed outer calls, not exclusive CPU\n",rank+1,atomic_load(&main_tid125),e->ordinal,kind,e->argument,e->caller,(unsigned long long)e->calls,e->ns/1e6,e->max/1e6);e->ns=e->calls=0;
    }
    fprintf(stderr,"[ORD125-SUM] overflow_calls=%u fast_calls_lt100us=%u ranked_min_us=100; completed slow calls only, active waits may complete in a later reporting interval\n",lost,atomic_exchange(&ord127_fast_calls,0));
}
static int snapshot(unsigned n,uint64_t now,uint64_t out[DJ_CATEGORIES]){
    TraceLane *r=&lanes[n];
    for(unsigned attempt=0;attempt<8;attempt++){
        unsigned a=atomic_load_explicit(&r->sequence,memory_order_acquire);if(a&1)continue;
        for(unsigned c=0;c<DJ_CATEGORIES;c++)out[c]=atomic_load_explicit(&r->total[c],memory_order_relaxed);
        uint64_t start=atomic_load_explicit(&r->start,memory_order_relaxed);unsigned c=atomic_load_explicit(&r->category,memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        if(a!=atomic_load_explicit(&r->sequence,memory_order_acquire))continue;
        /* If a scope switched after the present timestamp, do not pretend its
         * future totals belong to this frame. Rebaseline on the next frame. */
        if(!start||start>now)return 0;
        out[c]+=now-start;return 1;
    }return 0;
}
static void append123(char *line,size_t *used,const char *fmt,...);
void defjam_trace_frame(uint64_t now,uint64_t dt,int combat){
    if(!atomic_load_explicit(&enabled,memory_order_relaxed))return;
    static unsigned previous_valid;Hitch h={.frame=++frame_no,.dt=dt,.combat=combat};
    unsigned av=atomic_load(&active_sequence125);
    h.active_ordinal=atomic_load(&active_ordinal125);h.argument=atomic_load(&active_argument125);h.caller=atomic_load(&active_caller125);
    if((av&1)||av!=atomic_load(&active_sequence125))h.active_ordinal=0;
    for(unsigned l=0;l<LANES123;l++){
        uint64_t current[DJ_CATEGORIES];int valid=snapshot(l,now,current);
        if(valid){
            if(dt&&(previous_valid&(1u<<l))){
                h.valid|=1u<<l;
                for(unsigned c=0;c<DJ_CATEGORIES;c++){
                    if(current[c]<previous[l][c]){h.valid&=~(1u<<l);break;}
                    h.ns[l][c]=current[c]-previous[l][c];
                }
            }
            memcpy(previous[l],current,sizeof current);
        }
        if(valid)previous_valid|=1u<<l;else previous_valid&=~(1u<<l);
    }
    /* 0.1ms histogram, accumulated only inside uninterrupted combat. */
    static uint64_t combat_ns22,combat_n22,over34_22;
    static unsigned hist22[20001];static int prior_combat22;
    if(combat!=prior_combat22){memset(hist22,0,sizeof hist22);combat_ns22=combat_n22=over34_22=0;prior_combat22=combat;}
    if(combat&&dt){unsigned bin=dt/100000ull>20000?20000:(unsigned)(dt/100000ull);hist22[bin]++;combat_n22++;combat_ns22+=dt;over34_22+=dt>34000000ull;}
    if(combat&&combat_n22&&h.frame%240==0){
        uint64_t rank=(combat_n22*95+99)/100,cumulative=0;unsigned bin=0;
        while(bin<20000&&(cumulative+=hist22[bin])<rank)bin++;
        fprintf(stderr,"[ACCEPT22] frame=%"PRIu64" uninterrupted_combat_s=%.3f frames=%"PRIu64" p95_upper_ms=%.1f over34=%"PRIu64" timing_pass=%d; 0.1ms bins, final bin overflow; batch-count and device validation still required\n",h.frame,combat_ns22/1e9,combat_n22,(bin+1)/10.0,over34_22,combat_ns22>=180000000000ull&&bin<170&&!over34_22);
    }
    resources_frame22(h.frame,dt);
    static uint64_t sums22[LANES123][DJ_CATEGORIES],counts22[LANES123];
    for(unsigned l=0;l<LANES123;l++)if(h.valid&(1u<<l)){
        counts22[l]++;for(unsigned c=0;c<DJ_CATEGORIES;c++)sums22[l][c]+=h.ns[l][c];
    }
    if(h.frame%240==0){
        for(unsigned l=0;l<LANES123;l++){
            char line22[4096];size_t used22=0;append123(line22,&used22,"[PERF22] frame=%"PRIu64" lane=%s valid_frames=%"PRIu64,h.frame,role22[l],counts22[l]);
            for(unsigned c=0;c<DJ_CATEGORIES;c++)append123(line22,&used22," %s_ms_frame=%.4f",names[c],counts22[l]?sums22[l][c]/1e6/counts22[l]:0.0);
            fprintf(stderr,"%s\n",line22);
        }memset(sums22,0,sizeof sums22);memset(counts22,0,sizeof counts22);
    }
    pthread_mutex_lock(&guard);
    for(unsigned l=0;l<LANES123;l++)if(h.valid&(1u<<l))for(unsigned c=0;c<DJ_CATEGORIES;c++)sum[l][c]+=h.ns[l][c];
    if(dt>34000000ull){if(queued<HITCHES123)queue[queued++]=h;else dropped++;}
    pthread_mutex_unlock(&guard);
}
void defjam_trace_runtime(int64_t count,int64_t ms,int64_t bytes){atomic_store(&gc_count,count);atomic_store(&gc_ms,ms);atomic_store(&java_alloc,bytes);}
static void append123(char *line,size_t *used,const char *fmt,...){
    if(*used>=4095)return;va_list args;va_start(args,fmt);int n=vsnprintf(line+*used,4096-*used,fmt,args);va_end(args);
    if(n>0)*used+=(size_t)n<4096-*used?(size_t)n:4095-*used;
}
void defjam_trace_report(void){
    ordinal_report125();
    static Hitch copy[HITCHES123];uint64_t totals[LANES123][DJ_CATEGORIES];unsigned count,lost;
    pthread_mutex_lock(&guard);count=queued;lost=dropped;memcpy(copy,queue,count*sizeof queue[0]);queued=dropped=0;memcpy(totals,sum,sizeof sum);memset(sum,0,sizeof sum);pthread_mutex_unlock(&guard);
    const char *roles[LANES123]={"guest_main","render","mixer"};
    char line[4096];size_t used;
    for(unsigned i=0;i<count;i++){
        Hitch *h=&copy[i];used=0;append123(line,&used,"[HITCH123] frame=%"PRIu64" mode=%s wall_ms=%.3f lanes_valid=%u",h->frame,h->combat?"combat":"menu_or_video",h->dt/1e6,h->valid);
        if(h->active_ordinal)append123(line,&used," active_ordinal_at_sample=%u raw_key=%08X caller=%08X",h->active_ordinal-1,h->argument,h->caller);
        for(unsigned l=0;l<LANES123;l++)if(h->valid&(1u<<l)){
            append123(line,&used," %s={",roles[l]);for(unsigned c=0;c<DJ_CATEGORIES;c++)if(h->ns[l][c]>=100000ull)append123(line,&used,"%s:%.3f,",names[c],h->ns[l][c]/1e6);append123(line,&used,"}");
        }fprintf(stderr,"%s\n",line);
    }
    for(unsigned l=0;l<LANES123;l++){
        used=0;append123(line,&used,"[HITCH123-SUM] lane=%s",roles[l]);for(unsigned c=0;c<DJ_CATEGORIES;c++)append123(line,&used," %s_ms=%.3f",names[c],totals[l][c]/1e6);fprintf(stderr,"%s\n",line);
    }
    static int64_t old_count=-1,old_ms=-1;int64_t count_gc=atomic_load(&gc_count),ms=atomic_load(&gc_ms);
    fprintf(stderr,"[RUNTIME123] gc_count_delta=%"PRId64" gc_ms_delta=%"PRId64" java_allocated_total=%"PRId64"; interval-only ART counters, not per-frame GC attribution; -1=unavailable\n",old_count>=0&&count_gc>=old_count?count_gc-old_count:-1,old_ms>=0&&ms>=old_ms?ms-old_ms:-1,atomic_load(&java_alloc));old_count=count_gc;old_ms=ms;
    fprintf(stderr,"[HITCH123-SUM] enabled=%d frames_over34=%u dropped=%u; exclusive tagged wall time per lane; lanes overlap; unscoped_wall includes execution, preemption and untagged waits\n",atomic_load(&enabled),count,lost);
}
#endif
