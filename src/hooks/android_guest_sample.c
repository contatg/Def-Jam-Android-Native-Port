/* Opt-in CPU-time PC sampling of registered guest threads. The signal handler
 * only reads ucontext and updates lock-free counters. Symbol lookup/logging is
 * performed by the background monitor. No guest instrumentation or guest waits. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "platform/android_sched.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <errno.h>
#define SAMPLE_SLOTS 8192
typedef struct Sample { _Atomic uintptr_t pc; _Atomic unsigned count; } Sample;
static Sample samples[SAMPLE_SLOTS];
static _Atomic unsigned overflow;
static int sample_signal;
static _Atomic int enabled;
static pthread_key_t timer_key;
static _Thread_local int registered;
_Static_assert(ATOMIC_POINTER_LOCK_FREE==2&&ATOMIC_INT_LOCK_FREE==2,"Signal sampler requires lock-free atomics");
static void collect(int sig,siginfo_t *info,void *context){
    (void)sig;if(!info||info->si_code!=SI_TIMER||info->si_value.sival_int!=0x444A121)return;
    ucontext_t *u=context;uintptr_t pc=0;
#if defined(__aarch64__)
    pc=(uintptr_t)u->uc_mcontext.pc;
#elif defined(__x86_64__)
    pc=(uintptr_t)u->uc_mcontext.gregs[REG_RIP];
#endif
    if(!pc)return;
    unsigned hash=(unsigned)((pc>>2)^(pc>>15))&(SAMPLE_SLOTS-1);
    for(unsigned i=0;i<16;i++){
        Sample *s=&samples[(hash+i)&(SAMPLE_SLOTS-1)];uintptr_t value=atomic_load_explicit(&s->pc,memory_order_relaxed);
        if(!value){uintptr_t empty=0;atomic_compare_exchange_strong_explicit(&s->pc,&empty,pc,memory_order_relaxed,memory_order_relaxed);value=atomic_load_explicit(&s->pc,memory_order_relaxed);}
        if(value==pc){atomic_fetch_add_explicit(&s->count,1,memory_order_release);return;}
    }
    atomic_fetch_add_explicit(&overflow,1,memory_order_relaxed);
}
static void release_timer(void *p){if(p){timer_delete(*(timer_t*)p);free(p);}}
void defjam_guest_sampler_init(int on){
    if(!on)return;
#if !defined(__aarch64__) && !defined(__x86_64__)
    fprintf(stderr,"[GUEST121] sampler unavailable for this host architecture\n");return;
#endif
    if(pthread_key_create(&timer_key,release_timer))return;
    struct sigaction old,action={0};action.sa_sigaction=collect;action.sa_flags=SA_SIGINFO|SA_RESTART;sigemptyset(&action.sa_mask);
    for(int sig=SIGRTMIN+8;sig<SIGRTMAX;sig++){
        if(sigaction(sig,NULL,&old)||old.sa_handler!=SIG_DFL)continue;
        if(!sigaction(sig,&action,NULL)){sample_signal=sig;enabled=1;break;}
    }
    fprintf(stderr,"[GUEST121] sampler=%s rate=100Hz per guest-thread CPU second, sampled exclusive host PCs; no samples while sleeping\n",enabled?"on":"unavailable (no unused realtime signal)");
}
void defjam_guest_sampler_enter(void){
    if(!enabled||registered)return;
    registered=1;
    timer_t *timer=calloc(1,sizeof *timer);if(!timer)return;
    struct sigevent event={0};event.sigev_notify=SIGEV_THREAD_ID;event.sigev_signo=sample_signal;event.sigev_value.sival_int=0x444A121;
#if defined(__BIONIC__)
    event.sigev_notify_thread_id=(pid_t)syscall(SYS_gettid);
#else
    event._sigev_un._tid=(pid_t)syscall(SYS_gettid);
#endif
    if(timer_create(CLOCK_THREAD_CPUTIME_ID,&event,timer)){
        fprintf(stderr,"[GUEST121] tid=%ld timer unavailable errno=%d\n",(long)syscall(SYS_gettid),errno);free(timer);return;
    }
    struct itimerspec period={{0,10000000},{0,10000000}};
    if(pthread_setspecific(timer_key,timer)||timer_settime(*timer,0,&period,NULL)){
        pthread_setspecific(timer_key,NULL);release_timer(timer);return;
    }
    sigset_t set;sigemptyset(&set);sigaddset(&set,sample_signal);pthread_sigmask(SIG_UNBLOCK,&set,NULL);
}
typedef struct Rank { const char *name,*module;uintptr_t key;uint64_t count;int guest; } Rank;
static Rank ranking[SAMPLE_SLOTS];
static int compare(const void *a,const void *b){const Rank *x=a,*y=b;return x->count<y->count?1:x->count>y->count?-1:0;}
void defjam_guest_sampler_report(void){
    if(!enabled)return;
    unsigned count=0;uint64_t total=0,guest_total=0;
    for(unsigned i=0;i<SAMPLE_SLOTS;i++){
        unsigned n=atomic_exchange_explicit(&samples[i].count,0,memory_order_acquire);if(!n)continue;
        uintptr_t pc=atomic_load_explicit(&samples[i].pc,memory_order_relaxed);Dl_info info={0};dladdr((void*)pc,&info);
        const char *name=info.dli_sname?info.dli_sname:"<unresolved>";
        int guest=!strncmp(name,"sub_",4)||!strncmp(name,"xbe_entry",9);
        uintptr_t key=info.dli_saddr?(uintptr_t)info.dli_saddr:pc;
        unsigned j;for(j=0;j<count;j++)if(ranking[j].key==key)break;
        if(j==count)ranking[count++]=(Rank){name,info.dli_fname?info.dli_fname:"?",key,0,guest};
        ranking[j].count+=n;total+=n;if(guest)guest_total+=n;
    }
    qsort(ranking,count,sizeof ranking[0],compare);
    fprintf(stderr,"[GUEST121] total_samples=%llu guest_samples=%llu dropped_hash=%u; 10ms CPU/sample estimate, not exact function duration; unresolved symbols are not classified as guest\n",(unsigned long long)total,(unsigned long long)guest_total,atomic_exchange(&overflow,0));
    unsigned shown=0;
    for(unsigned i=0;i<count&&shown<20;i++)if(ranking[i].guest){
        fprintf(stderr,"[GUEST121-TOP] rank=%u function=%s samples=%llu cpu_est_ms=%.1f pct_guest=%.2f\n",++shown,ranking[i].name,(unsigned long long)ranking[i].count,ranking[i].count*10.0,guest_total?100.0*ranking[i].count/guest_total:0);
    }
    for(unsigned i=0;i<count&&i<10;i++)if(!ranking[i].guest)
        fprintf(stderr,"[GUEST121-HOST] function=%s module=%s samples=%llu\n",ranking[i].name,ranking[i].module,(unsigned long long)ranking[i].count);
    memset(ranking,0,count*sizeof ranking[0]);
}
#endif
