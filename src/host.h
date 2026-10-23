/*
 * host.h -- the few things this program's own files need from the host that
 * differ between Windows and everything else.
 *
 * On Windows this is <windows.h> and nothing more. Elsewhere it is the
 * runtime's Win32 vocabulary (win32_compat.h) plus the names that layer has
 * no reason to carry.
 */
#ifndef DEFJAM_HOST_H
#define DEFJAM_HOST_H

#include <stdint.h>

#if defined(_WIN32)

#include <windows.h>

#define HOST_TLS        __declspec(thread)
#define HOST_SEP        "\\"
#define HOST_SEP_CHAR   '\\'
/* The faulting instruction's address, from a vectored handler's context. */
#define HOST_PC(ctx)    ((uint64_t)((PCONTEXT)(ctx))->Rip)
/* Guest memory as host code may bulk-copy it; see the other definition. */
#define HOST_VIEW(p)    (p)

#else

#include <stdlib.h>
#include "win32_compat.h"
#include "mmio_trap.h"

#define HOST_TLS        __thread
#define HOST_SEP        "/"
#define HOST_SEP_CHAR   '/'
/* Here the context is the signal handler's ucontext. */
#define HOST_PC(ctx)    ((uint64_t)mmio_trap_pc(ctx))
/* Where the host page is larger than 4 KB, closing one device page closes its
 * neighbours, and a memcpy from one of those must go through the open view. */
#define HOST_VIEW(p)    mmio_trap_view(p)

#define _putenv_s(name, value) setenv((name), (value), 1)
static inline LONG64 InterlockedIncrement64(volatile LONG64 *p)
{ return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }

#endif

/* Tell the player something went wrong before there is a window to say it in. */
void host_fatal(const char *message);

#endif /* DEFJAM_HOST_H */
