#ifndef DEFJAM_ANDROID_THREAD_POLICY_H
#define DEFJAM_ANDROID_THREAD_POLICY_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__ANDROID__)
/* Initialize on SDL_main before any guest affinity is applied. Captures the
 * original allowed mask; never constructs a mask outside Android's limits.
 * RECOMP_NATIVE_CORES=0 means all originally allowed CPUs, independent of guest.
 * Configuration is immutable after initialization; restart to change it. */
void defjam_android_cpu_init(void);
int defjam_android_guest_select(const char *value,const void *allowed,void *selected,size_t size);
unsigned defjam_android_vertex_budget(void);
int defjam_android_native_core_count(void);
void defjam_android_native_thread_enter(const char *role, const char *function,
                                         uintptr_t entry);
void defjam_android_guest_thread_note(const char *role, const char *function,
                                       uintptr_t entry, uint32_t guest_entry,
                                       uint32_t bridge_id);
/* Opaque scheduler mask; no GNU-only types in this shared header. The size must
 * match the implementation's cpu_set_t. Uses an explicit Linux TID and publishes
 * the actual result in the next thread note. Invalid input never changes affinity. */
int defjam_android_guest_affinity_apply(const void *target, size_t target_size);
#endif
#ifdef __cplusplus
}
#endif
#endif
