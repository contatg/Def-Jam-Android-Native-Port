#ifndef XBOX_ROLLBACK_HOOKS_H
#define XBOX_ROLLBACK_HOOKS_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#if defined(__ANDROID__)
typedef int (*xbox_guest_write_callback131)(void *,size_t,int);
typedef struct {xbox_guest_write_callback131 callback;void *host;size_t length;int locked;} xbox_guest_write_scope131;
void xbox_set_guest_write_notify(xbox_guest_write_callback131 callback);
void xbox_rollback_watch_range131(uint32_t lo,uint32_t hi,uint32_t alias_delta);
void xbox_rollback_snapshot_begin131(void);
void xbox_rollback_snapshot_end131(void);
xbox_guest_write_scope131 xbox_guest_write_begin131(void *host,size_t length);
void xbox_guest_write_end131(xbox_guest_write_scope131 scope);
void xbox_notify_guest_write(void *host,size_t length);
void xbox_set_contig_free_defer(int (*callback)(uint32_t));
int xbox_ContiguousFreeNow(uint32_t addr);
int xbox_ContiguousAliasesReady(void);
void xbox_rollback_mmx_save131(uint64_t out[8]);
void xbox_rollback_mmx_restore131(const uint64_t in[8]);
#endif
#ifdef __cplusplus
}
#endif
#endif
