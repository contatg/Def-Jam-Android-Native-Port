#ifndef DEFJAM_ANDROID_VERTEX_POOL_H
#define DEFJAM_ANDROID_VERTEX_POOL_H
/* One dispatcher (the push-buffer executor), bounded persistent workers.
 * Callback ranges are disjoint. A dispatch returns only after every range
 * finishes; the caller may then reuse the immutable job and output buffer. */
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sched.h>
#include <stdio.h>
#if defined(__ANDROID__)
#include "platform/android_thread_policy.h"
#endif

typedef void (*NvVertexRange)(void *, uint32_t, uint32_t);
#define NV_VERTEX_WORKERS 7
#define NV_VERTEX_MIN_BATCH 2048u

typedef struct NvVertexPool NvVertexPool;
typedef struct {
    NvVertexPool *pool;
    unsigned id, start_generation;
    pthread_cond_t wake;
} NvVertexWorker;
struct NvVertexPool {
    pthread_mutex_t mutex;
    pthread_cond_t done;
    pthread_t threads[NV_VERTEX_WORKERS];
    NvVertexWorker worker[NV_VERTEX_WORKERS];
    unsigned count, pending, generation, participants;
    int initialized, stop, creation_limited;
    NvVertexRange callback;
    void *job;
    uint32_t total;
};
#define NV_VERTEX_POOL_INIT { .mutex=PTHREAD_MUTEX_INITIALIZER, \
    .done=PTHREAD_COND_INITIALIZER }

static void *nv_vertex_worker(void *arg)
{
    NvVertexWorker *worker = (NvVertexWorker *)arg;
    NvVertexPool *pool = worker->pool;
    unsigned seen = worker->start_generation;
#if defined(__ANDROID__)
    char role[16];
    snprintf(role, sizeof role, "vertex%u", worker->id);
    defjam_android_native_thread_enter(role, "nv_vertex_worker", (uintptr_t)nv_vertex_worker);
#endif
    pthread_mutex_lock(&pool->mutex);
    for (;;) {
        NvVertexRange callback;
        void *job;
        uint32_t total;
        unsigned parts;
        while (!pool->stop && seen == pool->generation)
            pthread_cond_wait(&worker->wake, &pool->mutex);
        if (pool->stop) break;
        seen = pool->generation;
        parts = pool->participants;
        if (worker->id + 1 >= parts) continue;
        callback = pool->callback; job = pool->job;
        total = pool->total;
        pthread_mutex_unlock(&pool->mutex);
        callback(job, (uint32_t)((uint64_t)total * worker->id / parts),
                     (uint32_t)((uint64_t)total * (worker->id + 1) / parts));
        pthread_mutex_lock(&pool->mutex);
        if (--pool->pending == 0) pthread_cond_signal(&pool->done);
    }
    pthread_mutex_unlock(&pool->mutex);
    return NULL;
}

static unsigned nv_vertex_pool_start(NvVertexPool *pool, unsigned requested)
{
    cpu_set_t allowed;
    unsigned permitted = 1;
    if (sched_getaffinity(0, sizeof allowed, &allowed) == 0)
        permitted = (unsigned)CPU_COUNT(&allowed);
    if (requested > permitted) requested = permitted;
    if (requested < 1) requested = 1;
    if (requested > NV_VERTEX_WORKERS + 1) requested = NV_VERTEX_WORKERS + 1;
    pthread_mutex_lock(&pool->mutex);
    pool->initialized = 1;
    for (unsigned k = pool->count; !pool->creation_limited && k + 1 < requested; ++k) {
        pool->worker[k].pool = pool; pool->worker[k].id = k;
        pool->worker[k].start_generation = pool->generation;
        if (pthread_cond_init(&pool->worker[k].wake, NULL) != 0) {
            pool->creation_limited = 1;
            break;
        }
        if (pthread_create(&pool->threads[k], NULL, nv_vertex_worker, &pool->worker[k]) != 0) {
            pthread_cond_destroy(&pool->worker[k].wake);
            pool->creation_limited = 1;
            break;
        }
        ++pool->count;
    }
    pthread_mutex_unlock(&pool->mutex);
    return pool->count + 1;
}

static unsigned nv_vertex_pool_run(NvVertexPool *pool, unsigned requested,
                                  uint32_t total, NvVertexRange callback, void *job)
{
    unsigned parts;
    if (requested < 2 || total < NV_VERTEX_MIN_BATCH) {
        callback(job, 0, total); return 1;
    }
    /* At least 1024 vertices per participant. A 2048-vertex job uses two,
     * instead of waking seven helpers for 256 vertices each. Grow the pool
     * only when a larger job actually arrives; idle helpers stay asleep. */
    unsigned useful = total / (NV_VERTEX_MIN_BATCH / 2);
    if (requested > useful) requested = useful;
    parts = nv_vertex_pool_start(pool, requested);
    if (parts > requested) parts = requested;
    if (parts < 2) { callback(job, 0, total); return 1; }
    pthread_mutex_lock(&pool->mutex);
    pool->callback = callback; pool->job = job; pool->total = total;
    pool->participants = parts;
    pool->pending = parts - 1; ++pool->generation;
    for (unsigned k = 0; k + 1 < parts; ++k) pthread_cond_signal(&pool->worker[k].wake);
    pthread_mutex_unlock(&pool->mutex);
    callback(job, (uint32_t)((uint64_t)total * (parts - 1) / parts), total);
    pthread_mutex_lock(&pool->mutex);
    while (pool->pending) pthread_cond_wait(&pool->done, &pool->mutex);
    pool->job = NULL; pool->callback = NULL;
    pthread_mutex_unlock(&pool->mutex);
    return parts;
}

/* Only call after the last dispatch returns. Reusable after stop. */
static void nv_vertex_pool_stop(NvVertexPool *pool)
{
    if (!pool->initialized) return;
    pthread_mutex_lock(&pool->mutex);
    pool->stop = 1;
    for (unsigned k = 0; k < pool->count; ++k) pthread_cond_signal(&pool->worker[k].wake);
    pthread_mutex_unlock(&pool->mutex);
    for (unsigned k = 0; k < pool->count; ++k) {
        pthread_join(pool->threads[k], NULL);
        pthread_cond_destroy(&pool->worker[k].wake);
    }
    pool->count = pool->pending = pool->generation = 0;
    pool->initialized = pool->stop = pool->creation_limited = 0;
    pool->participants = 0;
}
#endif
