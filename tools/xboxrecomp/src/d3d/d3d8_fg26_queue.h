#ifndef DEFJAM_FG26_QUEUE_H
#define DEFJAM_FG26_QUEUE_H
#if defined(__ANDROID__) || defined(FG26_HOST_TEST)
#include <pthread.h>
#include <stdatomic.h>
/* All host accesses to this shared VkQueue use the same short lock.
 * Never held across fences, acquisition, pacing or thread joins. */
static pthread_mutex_t fg26_queue_mutex=PTHREAD_MUTEX_INITIALIZER;
static _Atomic int fg26_queue_shared;
static int fg26_queue_enter(void) {
    int shared=atomic_load_explicit(&fg26_queue_shared,memory_order_acquire);
    if(shared)pthread_mutex_lock(&fg26_queue_mutex);
    return shared;
}
static void fg26_queue_leave(int shared) {
    if(shared)pthread_mutex_unlock(&fg26_queue_mutex);
}
static void fg26_stop(void);
#else
static int fg26_queue_enter(void){return 0;}
static void fg26_queue_leave(int held){(void)held;}
static void fg26_stop(void){}
#endif
#endif
