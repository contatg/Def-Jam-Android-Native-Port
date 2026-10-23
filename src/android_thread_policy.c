#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#if defined(__ANDROID__)
#include "platform/android_thread_policy.h"
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static pthread_once_t s_cpu_once = PTHREAD_ONCE_INIT;
static cpu_set_t s_original, s_native;
static int s_have_mask, s_request;
static _Atomic int s_render_allowed;
static _Thread_local int s_native_entered;
static _Thread_local int s_is_guest, s_callback_noted;

static void cpu_list(const cpu_set_t *set, char out[512])
{
    size_t used = 0;
    out[0] = 0;
    for (int cpu = 0; cpu < CPU_SETSIZE && used + 16 < 512; ++cpu)
        if (CPU_ISSET(cpu, set))
            used += (size_t)snprintf(out + used, 512 - used, "%s%d", used ? "," : "", cpu);
    if (!used) snprintf(out, 512, "unknown");
}

static void capture_cpu_policy(void)
{
    const char *value = getenv("RECOMP_NATIVE_CORES");
    char original[512], selected[512];
    int count = 0;
    s_request = value && value[0] >= '0' && value[0] <= '8' && !value[1] ? value[0] - '0' : 0;
    s_have_mask = sched_getaffinity(0, sizeof s_original, &s_original) == 0;
    if (!s_have_mask) {
        fprintf(stderr, "[ANDROID-THREAD] original affinity unavailable: errno=%d; no native mask applied\n", errno);
        return;
    }
    CPU_ZERO(&s_native);
    for (int cpu = CPU_SETSIZE - 1; cpu >= 0; --cpu)
        if (CPU_ISSET(cpu, &s_original) && (!s_request || count < s_request)) {
            CPU_SET(cpu, &s_native);
            ++count;
        }
    cpu_list(&s_original, original); cpu_list(&s_native, selected);
    fprintf(stderr, "[ANDROID-THREAD] native budget=%s; original_allowed={%s}; native_target={%s}; guest affinity is separate\n",
            s_request ? value : "auto", original, selected);
}

void defjam_android_cpu_init(void) { pthread_once(&s_cpu_once, capture_cpu_policy); }

int defjam_android_native_core_count(void)
{
    /* Never lazily initialize from a Java/UI getter, which could run before
     * SDL_main receives the configuration. The renderer publishes its actual
     * mask after applying it. Zero means it has not reported yet. */
    return atomic_load(&s_render_allowed);
}

static void thread_note(const char *role, const char *function, uintptr_t entry,
                        uint32_t guest_entry, uint32_t bridge_id, int result, int saved_errno)
{
    cpu_set_t applied;
    char list[512], name[16];
    snprintf(name, sizeof name, "dj-%s", role);
    pthread_setname_np(pthread_self(), name);
    CPU_ZERO(&applied);
    int have = sched_getaffinity(0, sizeof applied, &applied) == 0;
    cpu_list(&applied, list);
    fprintf(stderr, "[ANDROID-THREAD] role=%s function=%s linux_tid=%ld bridge_id=%u guest_entry=%08X host_entry=%p allowed={%s} mask_result=%d errno=%d\n",
            role, function, (long)syscall(SYS_gettid), bridge_id, guest_entry,
            (void *)entry, list, result, saved_errno);
    if (!strcmp(role, "render")) atomic_store(&s_render_allowed, have ? CPU_COUNT(&applied) : 0);
}

void defjam_android_native_thread_enter(const char *role, const char *function, uintptr_t entry)
{
    if (s_is_guest) {
        if (!s_callback_noted) {
            s_callback_noted = 1;
            fprintf(stderr, "[ANDROID-THREAD] native callback %s on guest Linux TID %ld; guest affinity retained\n",
                    function, (long)syscall(SYS_gettid));
        }
        return;
    }
    if (s_native_entered) return;
    s_native_entered = 1;
    defjam_android_cpu_init();
    int result = -1, saved_errno = ENOTSUP;
    if (s_have_mask && CPU_COUNT(&s_native)) {
        result = sched_setaffinity(0, sizeof s_native, &s_native);
        saved_errno = result ? errno : 0;
    }
    thread_note(role, function, entry, 0, 0, result, saved_errno);
}

void defjam_android_guest_thread_note(const char *role, const char *function, uintptr_t entry,
                                       uint32_t guest_entry, uint32_t bridge_id)
{
    /* Logs only. Never broadens a guest/interrupt thread onto the native mask. */
    s_is_guest = 1;
    thread_note(role, function, entry, guest_entry, bridge_id, 0, 0);
}
#endif
