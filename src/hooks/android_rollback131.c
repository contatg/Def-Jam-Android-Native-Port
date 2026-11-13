#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "android_rollback131.h"
#include "guest_window.h"
#include "platform/xbox_rollback_hooks.h"
#include "recomp/gen/recomp_types.h"
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <stdio.h>
#define RB_LO GUEST_WINDOW_LO
#define RB_HI GUEST_WINDOW_HI
#define RB_ALIAS 0x70000000ull
static _Atomic int poisoned;
static _Atomic unsigned active_faults;
void android_rollback131_snapshot_begin(void){xbox_rollback_snapshot_begin131();}
void android_rollback131_snapshot_end(void){xbox_rollback_snapshot_end131();}
static int configured_mode;
int android_rollback131_online(void){return configured_mode==2;}
void android_rollback131_configure(int mode){
    configured_mode=mode==1||mode==2?mode:0;
    const char *guest_cores=getenv("RECOMP_GUEST_CORES");
    if(configured_mode&&(sysconf(_SC_PAGESIZE)!=4096||(guest_cores&&strcmp(guest_cores,"1")))){
        fprintf(stderr,"[ROLLBACK131] unsupported page size/guest multicore: using existing lockstep without audio arena\n");configured_mode=0;
    }
    setenv("RECOMP_ROLLBACK",configured_mode?"on":"off",1);
    if(configured_mode){
        if(configured_mode==1){setenv("RECOMP_TEST_OBSERVATIONS","1",1);setenv("RECOMP_ROLLBACK_SOAK","6",1);}
        else unsetenv("RECOMP_ROLLBACK_SOAK");
    }else unsetenv("RECOMP_ROLLBACK_SOAK");
    fprintf(stderr,"[ROLLBACK131] mode=%d (0=off 1=offline-verify 2=experimental-online); implementation present, phone/hash validation required\n",configured_mode);
}

void android_rollback131_watch_heap(uint32_t lo, uint32_t hi)
{
    xbox_rollback_watch_range131(lo, hi, (uint32_t)RB_ALIAS);
}

static PVECTORED_EXCEPTION_HANDLER fault_fn;
static clockid_t game_clock;
static int clock_valid;
void android_rollback131_writer_enter(void){atomic_fetch_add_explicit(&active_faults,1,memory_order_acquire);}
void android_rollback131_writer_leave(void){atomic_fetch_sub_explicit(&active_faults,1,memory_order_release);}
void android_rollback131_drain(void){
    struct timespec pause={0,500000};
    while(atomic_load_explicit(&active_faults,memory_order_acquire))nanosleep(&pause,NULL);
}
void android_rollback131_poison(void){atomic_store_explicit(&poisoned,1,memory_order_release);}
int android_rollback131_healthy(void){return !atomic_load_explicit(&poisoned,memory_order_acquire);}
void *android_rollback131_canonical(void *host){
    uintptr_t a=(uintptr_t)host,base=(uintptr_t)g_xbox_mem_offset;
    if (a >= base + RB_LO + RB_ALIAS && a < base + RB_HI + RB_ALIAS)
        a -= RB_ALIAS;
    return (void *)a;
}
int android_rollback131_basic_available(void){return sysconf(_SC_PAGESIZE)==4096 && xbox_ContiguousAliasesReady() && android_rollback131_healthy();}
int android_rollback131_available(void){
    if(sysconf(_SC_PAGESIZE)!=4096 || !xbox_ContiguousAliasesReady()){
        fprintf(stderr,"[ROLLBACK131] unavailable: requires4096-byte host pages and verified contiguous/tiled aliases; lockstep retained\n");return 0;
    }
    cpu_set_t mask;int count=0;
    if(sched_getaffinity(0,sizeof mask,&mask))return 0;
    for(int n=0;n<CPU_SETSIZE;n++)if(CPU_ISSET(n,&mask))count++;
    if(count!=1){fprintf(stderr,"[ROLLBACK131] unavailable: guest must remain on one allowed CPU; native worker affinities unchanged\n");return 0;}
    return android_rollback131_healthy();
}
BOOL android_rollback131_protect(void *host,SIZE_T length,DWORD mode,PDWORD old){
    uintptr_t a=(uintptr_t)host,base=(uintptr_t)g_xbox_mem_offset;
    if(!length || (a&4095) || (length&4095) || a<base+RB_LO || a>base+RB_HI || length>base+RB_HI-a)return FALSE;
    int prot=mode==PAGE_READONLY?PROT_READ:mode==PAGE_READWRITE?PROT_READ|PROT_WRITE:0;
    if(!prot)return FALSE;
    /* Linux syscalls only in a fault: no allocation, logging or mutex. */
    long first=syscall(SYS_mprotect,(void *)a,length,prot);
    long second=syscall(SYS_mprotect,(void *)(a+RB_ALIAS),length,prot);
    if(first||second){
        syscall(SYS_mprotect,(void *)a,length,PROT_READ|PROT_WRITE);
        syscall(SYS_mprotect,(void *)(a+RB_ALIAS),length,PROT_READ|PROT_WRITE);
        android_rollback131_poison();return FALSE;
    }
    if(old)*old=PAGE_READWRITE;return TRUE;
}
static int fault_watch(void *address,void *ctx){
    uintptr_t a=(uintptr_t)android_rollback131_canonical(address),base=(uintptr_t)g_xbox_mem_offset;
    if(a<base+RB_LO||a>=base+RB_HI||!fault_fn)return 0;
    uintptr_t pc=(uintptr_t)mmio_trap_pc(ctx);
    if((pc>=base+RB_LO&&pc<base+RB_HI)||(pc>=base+RB_LO+RB_ALIAS&&pc<base+RB_HI+RB_ALIAS))return 0; /* not an executable heap */
    EXCEPTION_RECORD record={0};EXCEPTION_POINTERS pointers={0};
    record.ExceptionCode=EXCEPTION_ACCESS_VIOLATION;record.NumberParameters=2;
    record.ExceptionInformation[0]=1;record.ExceptionInformation[1]=a;
    record.ExceptionAddress=(void *)pc;pointers.ExceptionRecord=&record;
    atomic_fetch_add_explicit(&active_faults,1,memory_order_acquire);
    LONG result=fault_fn(&pointers);
    atomic_fetch_sub_explicit(&active_faults,1,memory_order_release);
    return result==EXCEPTION_CONTINUE_EXECUTION && android_rollback131_healthy();
}
PVOID android_rollback131_install(PVECTORED_EXCEPTION_HANDLER fn){
    if(sysconf(_SC_PAGESIZE)!=4096)return NULL;
    fault_fn=fn;
    if(mmio_write_watch_claim(fault_watch)){fault_fn=NULL;return NULL;}
    return (PVOID)(uintptr_t)1;
}
HANDLE android_rollback131_game_clock(void){
    clock_valid=pthread_getcpuclockid(pthread_self(),&game_clock)==0;
    if(!clock_valid){android_rollback131_poison();return NULL;}
    return (HANDLE)(uintptr_t)1;
}
BOOL android_rollback131_clock(HANDLE handle,uint64_t *ns){
    struct timespec t;if(!handle||!clock_valid||clock_gettime(game_clock,&t)){android_rollback131_poison();return FALSE;}
    *ns=(uint64_t)t.tv_sec*1000000000ull+t.tv_nsec;return TRUE;
}
#endif
