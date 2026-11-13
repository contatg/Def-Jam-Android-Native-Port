/* Android/POSIX adaptation, not a Win32 VEH shim. */
#ifndef ANDROID_ROLLBACK131_H
#define ANDROID_ROLLBACK131_H
#include "host.h"
#include <stdint.h>
void android_rollback131_configure(int mode);
void android_rollback131_watch_heap(uint32_t lo, uint32_t hi);
int android_rollback131_online(void);
int android_rollback131_available(void);
int android_rollback131_basic_available(void);
int android_rollback131_healthy(void);
void android_rollback131_poison(void);
void android_rollback131_drain(void);
void android_rollback131_writer_enter(void);
void android_rollback131_writer_leave(void);
void android_rollback131_snapshot_begin(void);
void android_rollback131_snapshot_end(void);
void *android_rollback131_canonical(void *host);
BOOL android_rollback131_protect(void *host,SIZE_T length,DWORD mode,PDWORD old);
PVOID android_rollback131_install(PVECTORED_EXCEPTION_HANDLER fn);
HANDLE android_rollback131_game_clock(void);
BOOL android_rollback131_clock(HANDLE handle,uint64_t *ns);

#ifdef DEFJAM_ROLLBACK131_PC_CORE
static inline char android_rollback131_exchange8(volatile char *p,char v){return __atomic_exchange_n(p,v,__ATOMIC_ACQ_REL);}
#define InterlockedExchange8 android_rollback131_exchange8
#define VirtualProtect android_rollback131_protect
#define AddVectoredExceptionHandler(priority,fn) android_rollback131_install(fn)
#define OpenThread(access,inherit,tid) android_rollback131_game_clock()
#define QueryThreadCycleTime android_rollback131_clock
#define THREAD_QUERY_LIMITED_INFORMATION 0
#ifndef ULONG64
#define ULONG64 uint64_t
#endif
#endif /* PC core vocabulary only */
#endif
