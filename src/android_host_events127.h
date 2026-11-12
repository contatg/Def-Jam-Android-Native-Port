#ifndef DEFJAM_HOST_EVENTS127_H
#define DEFJAM_HOST_EVENTS127_H
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
/* Process-lifetime wake state: SDL event watchers can still be unwinding when
 * removed. No SDL/window calls from a watch callback and no guest clocks here. */
typedef struct {pthread_mutex_t mutex;pthread_cond_t wake;clockid_t clock;uint64_t sequence;} HostEvents127;
static HostEvents127 host_events127;
static pthread_once_t host_once127=PTHREAD_ONCE_INIT;
static void host_events_init127(void){
    pthread_condattr_t attr;pthread_mutex_init(&host_events127.mutex,NULL);
    pthread_condattr_init(&attr);host_events127.clock=CLOCK_REALTIME;
    if(!pthread_condattr_setclock(&attr,CLOCK_MONOTONIC))host_events127.clock=CLOCK_MONOTONIC;
    pthread_cond_init(&host_events127.wake,&attr);pthread_condattr_destroy(&attr);
}
static uint64_t host_event_sequence127(void){
    pthread_once(&host_once127,host_events_init127);
    pthread_mutex_lock(&host_events127.mutex);uint64_t seq=host_events127.sequence;pthread_mutex_unlock(&host_events127.mutex);return seq;
}
static void host_event_wake127(void){
    pthread_once(&host_once127,host_events_init127);
    pthread_mutex_lock(&host_events127.mutex);host_events127.sequence++;pthread_cond_signal(&host_events127.wake);pthread_mutex_unlock(&host_events127.mutex);
}
static void host_event_pause127(uint64_t seen,int full_batch){
    if(full_batch){const struct timespec delay={0,250000};nanosleep(&delay,NULL);return;}
    pthread_mutex_lock(&host_events127.mutex);
    if(seen==host_events127.sequence){
        struct timespec deadline;clock_gettime(host_events127.clock,&deadline);
        deadline.tv_nsec+=4000000;if(deadline.tv_nsec>=1000000000){deadline.tv_sec++;deadline.tv_nsec-=1000000000;}
        while(seen==host_events127.sequence){int rc=pthread_cond_timedwait(&host_events127.wake,&host_events127.mutex,&deadline);if(rc)break;}
    }
    pthread_mutex_unlock(&host_events127.mutex);
}
#endif
