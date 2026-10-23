/*
 * mmio_trap.h -- trapped device registers on a POSIX host.
 *
 * On Windows a device page is left PAGE_NOACCESS and a vectored handler
 * decodes the faulting x86-64 instruction (mmio_decode.h). This is the same
 * arrangement for hosts where that cannot be reused as it stands:
 *
 *   - the fault arrives as SIGSEGV or SIGBUS with a ucontext;
 *   - on arm64 the faulting instruction is an A64 load or store, decoded
 *     here;
 *   - the host page may be larger than the 4 KB the console's devices are
 *     laid out in (16 KB on Apple Silicon). Protecting one device page then
 *     closes its neighbours too, so an access to a neighbour is completed
 *     against a second, always-open view of the same memory.
 *
 * The callbacks have the mmio_decode.h signatures, so a device model serves
 * both.
 */
#ifndef MMIO_TRAP_H
#define MMIO_TRAP_H

#include <stddef.h>
#include <stdint.h>

#if !defined(_WIN32)

typedef uint64_t (*mmio_trap_read_fn)(void *dev, uint32_t off, int size);
typedef void     (*mmio_trap_write_fn)(void *dev, uint32_t off, uint64_t val, int size);

/* Trap [addr, addr + len). off in the callbacks is relative to addr. Both
 * must be multiples of 4096; the host pages that contain the range are closed
 * and every other byte in them keeps behaving as memory. Returns 0, or -1
 * with errno set. Installs this file's signal handlers on first use. */
int mmio_trap_add(void *addr, size_t len, void *dev,
                  mmio_trap_read_fn rd, mmio_trap_write_fn wr);

/* Reopen a range added or closed here. */
int mmio_trap_remove(void *addr);

/*
 * The same pieces for a program that owns the signal handler itself and
 * routes faults to its device models by address, as the Win32 hosts do with
 * their vectored handler.
 *
 *   mmio_trap_close     close a 4 KB-granular range without callbacks
 *   mmio_trap_emulate   service the faulting instruction in uctx against one
 *                       device: the access is at off, and a second element of
 *                       a pair follows it. 1 if serviced and stepped over.
 *   mmio_trap_neighbour 1 if fault only shared a host page with a closed
 *                       range and was completed as memory.
 *   mmio_trap_pc        the faulting instruction's address, for reports.
 */
int       mmio_trap_close(void *addr, size_t len);
/* Where host code can reach the memory at addr with a bulk copy: the open
 * view when a closed range shares its host page, addr itself otherwise. A
 * memcpy through the guest's view would fault in an instruction nothing here
 * decodes. */
void     *mmio_trap_view(void *addr);
int       mmio_trap_emulate(void *uctx, uint32_t off, void *dev,
                            mmio_trap_read_fn rd, mmio_trap_write_fn wr);
int       mmio_trap_neighbour(void *uctx, uintptr_t fault);
uintptr_t mmio_trap_pc(void *uctx);

/* Accesses completed against the open view because they only shared a host
 * page with a device. A cost to watch on 16 KB hosts; always 0 on 4 KB ones. */
uint64_t mmio_trap_neighbour_count(void);

/* One decoded A64 load or store. Exposed for the decoder's own tests. */
typedef struct {
    int      size;        /* bytes per element: 1, 2, 4, 8 */
    int      is_load;
    int      sign_extend; /* load sign-extends ... */
    int      dest_64;     /* ... to 64 bits rather than 32 */
    int      rt, rt2;     /* rt2 < 0 unless a pair; 31 is the zero register */
    int      rn;          /* base; 31 is sp */
    int      writeback;   /* base register is updated ... */
    int64_t  wb_delta;    /* ... by this */
} mmio_a64_access;

/* 1 if insn is a load or store this file services. */
int mmio_a64_decode(uint32_t insn, mmio_a64_access *out);

#endif /* !_WIN32 */
#endif /* MMIO_TRAP_H */
