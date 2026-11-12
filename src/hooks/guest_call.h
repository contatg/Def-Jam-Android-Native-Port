/* Calling a translated game function from host code, on the game's own thread.
 *
 * A translated function reads its arguments from the guest stack (g_esp points
 * at the return address on entry), `this` from g_ecx, and returns in g_eax;
 * its `ret` is `g_esp += 4` (+ N for `ret N`). So a call from a hook is: push the
 * arguments and a return address nothing will jump to, set ecx, call, and put
 * every register back as it was -- the hook's own caller must not see that
 * anything ran. Only safe from a hook that runs on a game thread (an _enter
 * hook, the front end's FuncCall), never from a host thread: the registers are
 * per thread.
 *
 * Used by fe_inject.c (front-end commands) and crowd_control.c (re-seeding the
 * crowd's generators). Test controls only. */

#ifndef DEFJAM_GUEST_CALL_H
#define DEFJAM_GUEST_CALL_H

#include <stdint.h>
#include "recomp/gen/recomp_types.h"

typedef struct GuestRegs {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    int fp_top;
} GuestRegs;

static inline void guest_regs_save(GuestRegs *r)
{
    r->eax = g_eax; r->ecx = g_ecx; r->edx = g_edx; r->ebx = g_ebx;
    r->esp = g_esp; r->ebp = g_ebp; r->esi = g_esi; r->edi = g_edi;
    r->fp_top = g_fp_top;
}

static inline void guest_regs_restore(const GuestRegs *r)
{
    g_eax = r->eax; g_ecx = r->ecx; g_edx = r->edx; g_ebx = r->ebx;
    g_esp = r->esp; g_ebp = r->ebp; g_esi = r->esi; g_edi = r->edi;
    g_fp_top = r->fp_top;
}

/* fn(args...) with this = ecx; returns what it left in eax. Arguments are given
 * left to right, as in the source. */
static inline uint32_t guest_call(void (*fn)(void), uint32_t ecx, int argc, const uint32_t *args)
{
    GuestRegs saved;
    uint32_t result;
    int i;

    guest_regs_save(&saved);
    for (i = argc - 1; i >= 0; i--) {
        g_esp -= 4;
        MEM32(g_esp) = args[i];
    }
    g_esp -= 4;
    MEM32(g_esp) = 0;               /* the return address: the call returns to C, not to it */
    g_ecx = ecx;
    fn();
    result = g_eax;
    guest_regs_restore(&saved);     /* cdecl or `ret N`, the stack is put back the same way */
    return result;
}

#endif
