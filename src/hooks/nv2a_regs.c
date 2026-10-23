/*
 * The few NV2A register blocks that cannot be plain memory.
 *
 * The title's Direct3D is statically linked into the XBE, so it drives the
 * GPU's registers itself rather than going through anything this runtime
 * provides. Nearly all of that works against the zeroed memory the runtime
 * maps at 0xFD000000, because nearly all of it is state the title writes and
 * reads back. Two kinds of register are not:
 *
 *   - one whose bit the hardware clears when an operation finishes, and
 *   - one that counts on its own.
 *
 * Both hang a title that waits on them. This file traps the pages they live on
 * and models just those registers; everything else on the trapped pages keeps
 * whatever is written to it, exactly as the memory did.
 *
 * ── 0xFD100000, PFB: the write-back-cache flush ────────────────────────────
 *
 *     0x0021EA89   mov  ecx, [eax+0x100410]     ; NV_PFB_WBC
 *                  or   ecx, 0x10000            ; NV_PFB_WBC_FLUSH
 *                  mov  [eax+0x100410], ecx     ; ask for the flush
 *     0x0021EAB0   test dword ptr [eax+0x100410], 0x10000
 *                  jne  0x0021EAB0              ; spin until it completes
 *
 * `eax` is 0xFD000000, read out of the device object, so this is 0xFD100410.
 * Hardware clears FLUSH when the cache has been written back. Against memory
 * the bit stays set and the main thread never leaves the loop. There is no
 * cache here to write back, so the honest answer is that the flush is already
 * done: the bit reads back clear.
 *
 * The symptom was easy to misread, which is worth recording. The rendering
 * thread is a different thread, so it kept resubmitting the frame it already
 * had: the loading screen stayed up and kept animating while the thread that
 * would advance the game was wedged in a two-instruction loop.
 *
 * ── 0xFD009000, PTIMER: the free-running clock ─────────────────────────────
 *
 * PTIMER counts continuously on hardware and is read as a 56-bit tick count
 * through TIME_0 and TIME_1. Against zeroed memory it reads zero forever, so
 * any "wait until the clock has advanced" is a hang, and any elapsed-time
 * calculation is zero. This is also the block xemu's open issue for this title
 * (#1124, "an unsupported PTimer operation") points at, so what the title does
 * with it is worth knowing regardless of whether it is our blocker.
 *
 * The counter here is derived from the host's performance counter, scaled by
 * the numerator and denominator the title programs, which is how the hardware
 * defines the rate. Reads are counted so a run can answer the prior question:
 * does this title read the clock at all, or only disable its interrupt?
 *
 * PFIFO's push-buffer pointers at 0xFD003000, which the title drives every
 * frame, are on another page and stay plain memory. PGRAPH at 0xFD400000 is
 * trapped for its write-1-to-clear interrupt status (see nv2a_regs_init).
 */

#include "host.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "mmio_decode.h"

#define NV2A_PAGE       0x1000u

#define PFB_BASE        0xFD100000u
#define PFB_WBC         0x410u       /* NV_PFB_WBC */
#define PFB_WBC_FLUSH   0x00010000u  /* hardware clears it when the flush ends */

#define PTIMER_BASE     0xFD009000u
#define PTIMER_INTR     0x100u       /* NV_PTIMER_INTR_0 */
#define PTIMER_INTR_EN  0x140u       /* NV_PTIMER_INTR_EN_0 */
#define PTIMER_NUM      0x200u       /* NV_PTIMER_NUMERATOR */
#define PTIMER_DENOM    0x210u       /* NV_PTIMER_DENOMINATOR */
#define PTIMER_TIME_0   0x400u       /* low 27 bits of the tick count, << 5 */
#define PTIMER_TIME_1   0x410u       /* the bits above those */
#define PTIMER_ALARM    0x420u

typedef struct NvPage NvPage;
struct NvPage {
    const char *name;
    uint32_t    base;
    uint8_t     shadow[NV2A_PAGE];
    uint32_t    reads, writes, undecoded, serviced;
    /* Registers this page answers itself rather than from the shadow. */
    int       (*special_read)(NvPage *p, uint32_t off, uint32_t *out);
    void      (*after_write)(NvPage *p, uint32_t off, int size);
    /* One write-1-to-clear status register on the page, 0 for none. */
    uint32_t    w1c_off;
};

#define PGRAPH_BASE     0xFD400000u
#define PGRAPH_INTR     0x100u       /* NV_PGRAPH_INTR, write-1-to-clear */
#define PCRTC_BASE      0xFD600000u
#define PCRTC_INTR      0x100u       /* NV_PCRTC_INTR_0, write-1-to-clear */

/* The runtime's interrupt model (kernel_bridge.c): recomputes PMC_INTR_0 from
 * the units' status, so an acknowledgement drops the summary at once. */
extern void xbox_Nv2aIntrUpdate(void);

/* The runtime reaches trapped pages through these once they are registered
 * (tools/xboxrecomp, xbox_memory_layout.c); its own threads would fault on a
 * direct access. */
extern void xbox_Nv2aSetPageHooks(uint32_t page_va, uint32_t (*rd)(uint32_t va),
                                  void (*wr)(uint32_t va, uint32_t value));

static NvPage   s_pages[5];
static int      s_page_count;
static int      s_trace;          /* RECOMP_NV2A_REG_TRACE */
static ptrdiff_t s_mem_offset;    /* guest VA -> host, for unowned pages */

static NvPage *page_for(uint32_t va)
{
    int i;
    for (i = 0; i < s_page_count; i++)
        if (va >= s_pages[i].base && va < s_pages[i].base + NV2A_PAGE)
            return &s_pages[i];
    return NULL;
}

/* ── PFB ──────────────────────────────────────────────────────────────── */

static void pfb_after_write(NvPage *p, uint32_t off, int size)
{
    uint32_t *wbc;

    if (off > PFB_WBC || off + (uint32_t)size <= PFB_WBC)
        return;
    wbc = (uint32_t *)&p->shadow[PFB_WBC];
    if (!(*wbc & PFB_WBC_FLUSH))
        return;
    *wbc &= ~PFB_WBC_FLUSH;
    if (++p->serviced <= 3) {
        fprintf(stderr, "  [PFB] write-back-cache flush answered (#%u)\n",
                p->serviced);
        fflush(stderr);
    }
}

/* ── PTIMER ───────────────────────────────────────────────────────────── */

static LARGE_INTEGER s_qpc_freq, s_qpc_start;

/* The tick count the hardware would be showing now.
 *
 * Hardware runs PTIMER off the GPU clock divided by DENOMINATOR and multiplied
 * by NUMERATOR, which is how a title asks for the units it wants. Both default
 * to one here, which gives the host's own performance-counter rate: monotonic
 * and roughly a microsecond, which is what a title measuring elapsed time
 * needs. A title that programs the ratio gets it applied. */
static uint64_t ptimer_ticks(NvPage *p)
{
    LARGE_INTEGER now;
    uint32_t num = *(uint32_t *)&p->shadow[PTIMER_NUM];
    uint32_t den = *(uint32_t *)&p->shadow[PTIMER_DENOM];
    uint64_t elapsed;

    QueryPerformanceCounter(&now);
    elapsed = (uint64_t)(now.QuadPart - s_qpc_start.QuadPart);

    if (num && den)
        elapsed = elapsed * num / den;
    return elapsed;
}

static int ptimer_special_read(NvPage *p, uint32_t off, uint32_t *out)
{
    uint64_t t;

    if (off != PTIMER_TIME_0 && off != PTIMER_TIME_1)
        return 0;
    t = ptimer_ticks(p);
    /* TIME_0 carries the low bits shifted up by five, TIME_1 everything above
     * them, which is the split the hardware documents and xemu implements. */
    *out = (off == PTIMER_TIME_0) ? (uint32_t)((t & 0x07FFFFFu) << 5)
                                  : (uint32_t)(t >> 27);
    p->serviced++;
    if (p->serviced <= 3 || (p->serviced % 10000) == 0) {
        fprintf(stderr, "  [PTIMER] clock read #%u (TIME_%d = 0x%08X)\n",
                p->serviced, off == PTIMER_TIME_0 ? 0 : 1, *out);
        fflush(stderr);
    }
    return 1;
}

/* An interrupt status register was acknowledged: the summary follows it, as
 * the hardware's does. Without this the deferred routine that acknowledged it
 * re-reads a summary still naming the unit, and services it again. */
static void intr_after_write(NvPage *p, uint32_t off, int size)
{
    (void)p; (void)size;
    if (off == 0x100u)
        xbox_Nv2aIntrUpdate();
}

/* ── the trap ─────────────────────────────────────────────────────────── */

static uint64_t nv_read(void *dev, uint32_t off, int size)
{
    NvPage *p = (NvPage *)dev;
    uint64_t v = 0;
    uint32_t special;
    int i;

    p->reads++;
    if (p->special_read && size == 4 && p->special_read(p, off, &special))
        return special;
    for (i = 0; i < size && off + i < NV2A_PAGE; i++)
        v |= (uint64_t)p->shadow[off + i] << (8 * i);
    if (s_trace)
        fprintf(stderr, "  [%s] read +0x%03X -> %0*llX\n",
                p->name, off, size * 2, (unsigned long long)v);
    return v;
}

static void nv_write(void *dev, uint32_t off, uint64_t val, int size)
{
    NvPage *p = (NvPage *)dev;
    int i;

    p->writes++;
    if (p->w1c_off && off == p->w1c_off && size == 4) {
        /* Write-1-to-clear: the bits written are the ones acknowledged. */
        uint32_t *reg = (uint32_t *)&p->shadow[off];
        *reg &= ~(uint32_t)val;
        if (s_trace)
            fprintf(stderr, "  [%s] ack +0x%03X with %08llX -> %08X\n",
                    p->name, off, (unsigned long long)val, *reg);
        if (p->after_write)
            p->after_write(p, off, size);
        return;
    }
    for (i = 0; i < size && off + i < NV2A_PAGE; i++)
        p->shadow[off + i] = (uint8_t)(val >> (8 * i));
    if (s_trace)
        fprintf(stderr, "  [%s] write +0x%03X = %0*llX\n",
                p->name, off, size * 2, (unsigned long long)val);
    if (p->after_write)
        p->after_write(p, off, size);
}

int nv2a_regs_owns_address(uint32_t xbox_va)
{
    return page_for(xbox_va) != NULL;
}

/* The runtime's side of a trapped page: the hardware setting a register, so
 * a plain store even where the guest's writes are write-1-to-clear. */
static uint32_t host_read(uint32_t va)
{
    NvPage *p = page_for(va);
    if (!p)
        return *(volatile uint32_t *)((uintptr_t)va + (uintptr_t)s_mem_offset);
    return *(uint32_t *)&p->shadow[(va - p->base) & (NV2A_PAGE - 4)];
}

static void host_write(uint32_t va, uint32_t value)
{
    NvPage *p = page_for(va);
    if (!p) {
        *(volatile uint32_t *)((uintptr_t)va + (uintptr_t)s_mem_offset) = value;
        return;
    }
    *(uint32_t *)&p->shadow[(va - p->base) & (NV2A_PAGE - 4)] = value;
}

int nv2a_regs_handle_mmio(void *ctx, uint32_t xbox_va)
{
    NvPage *p = page_for(xbox_va);
    int ok;

    if (!p)
        return 0;
    {
        /* RECOMP_NV2A_REG_WATCH=<va>: every guest access to one register on a
         * trapped page, with the host RIP that made it (resolve it against
         * the build's map). The hardware watchpoints cannot see these pages:
         * the access faults before it happens. */
        static uint32_t watch = 1;
        uint64_t rip = HOST_PC(ctx);
        if (watch == 1) {
            const char *w = getenv("RECOMP_NV2A_REG_WATCH");
            watch = w ? (uint32_t)strtoul(w, NULL, 0) : 0;
        }
        ok = mmio_emulate((PCONTEXT)ctx, xbox_va - p->base, p, nv_read, nv_write);
        if (watch && (xbox_va & ~3u) == watch) {
            static unsigned n;
            if (n++ < 400)
                fprintf(stderr, "  [%s] watch 0x%08X now 0x%08X from host RIP 0x%llX (#%u)\n",
                        p->name, xbox_va, *(uint32_t *)&p->shadow[(xbox_va - p->base) & (NV2A_PAGE - 4)],
                        (unsigned long long)rip, n);
        }
    }
    if (!ok && p->undecoded++ < 10) {
        const uint8_t *ip = (const uint8_t *)(uintptr_t)HOST_PC(ctx);
        fprintf(stderr, "  [%s] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                p->name, xbox_va - p->base, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
}

/* Trap the pages, seeding each shadow from what is already there so anything
 * the runtime configured beforehand survives. Harmless to call twice. */
void nv2a_regs_init(ptrdiff_t mem_offset)
{
    struct { const char *name; uint32_t base;
             int (*rd)(NvPage *, uint32_t, uint32_t *);
             void (*wr)(NvPage *, uint32_t, int);
             uint32_t w1c; } want[] = {
        { "PFB",    PFB_BASE,    NULL,                 pfb_after_write, 0 },
        /* PGRAPH, for its interrupt status. D3D's handler acknowledges a
         * software method by writing back what it read and loops while any
         * remains; as plain memory nothing cleared, the deferred routine
         * spun, and the frame timer's alarm was never delivered again. The
         * page is read in D3D's fence waits, so this costs a fault per read
         * there, which is where correctness matters more than speed. */
        { "PGRAPH", PGRAPH_BASE, NULL,                 intr_after_write, PGRAPH_INTR },
        /* PCRTC and PTIMER, for the same reason: their interrupt status is
         * write-1-to-clear, and the runtime's interrupts are level-triggered
         * on it (kernel_bridge.c, patch 0020). As plain memory an
         * acknowledgement left the status set, and Direct3D's deferred routine,
         * which loops and re-reads the summary, could service one frame alarm
         * twice -- the second time with its timer slot empty, which it takes
         * for a spurious alarm and answers by switching the alarm off. The
         * runtime reaches both pages through the register hooks. */
        { "PCRTC",  PCRTC_BASE,  NULL,                 intr_after_write, PCRTC_INTR },
        { "PTIMER", PTIMER_BASE, NULL,                 intr_after_write, PTIMER_INTR },
        /* PTIMER used to be modelled here. It moved into the runtime's timer
         * thread (patch 0007) once it turned out that this title's frame clock
         * is the PTIMER alarm: raising a guest interrupt needs the guest stack
         * and TIB that thread owns, and a trapped page can only be serviced
         * from the thread that faulted on it. Left as plain memory here so
         * both halves see the same registers. */
    };
    size_t i;

    if (s_page_count)
        return;

    s_trace = getenv("RECOMP_NV2A_REG_TRACE") != NULL;
    s_mem_offset = mem_offset;
    QueryPerformanceFrequency(&s_qpc_freq);
    QueryPerformanceCounter(&s_qpc_start);

    for (i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        void *native = (void *)((uintptr_t)want[i].base + (uintptr_t)mem_offset);
        NvPage *p = &s_pages[s_page_count];
        DWORD old = 0;

        memcpy(p->shadow, HOST_VIEW(native), NV2A_PAGE);
        p->name         = want[i].name;
        p->base         = want[i].base;
        p->special_read = want[i].rd;
        p->after_write  = want[i].wr;
        p->w1c_off      = want[i].w1c;
        /* Owned before it is protected: the runtime's threads poll these
         * registers flat out, and one that touched the page in between
         * faulted -- the occasional start-up crash in nv2a_ack_thread. */
        s_page_count++;
        xbox_Nv2aSetPageHooks(want[i].base, host_read, host_write);
        if (!VirtualProtect(native, NV2A_PAGE, PAGE_NOACCESS, &old)) {
            fprintf(stderr, "  [%s] could not trap 0x%08X (error %lu); "
                            "it stays plain memory\n",
                    want[i].name, want[i].base, GetLastError());
            /* The hook stays registered and falls through to memory. */
            s_page_count--;
            continue;
        }
    }

    if (s_page_count)
        fprintf(stderr, "  NV2A: %d register page(s) modelled "
                        "(cache flushes complete, PTIMER counts)\n",
                s_page_count);
    fflush(stderr);
}

void nv2a_regs_report(void)
{
    int i;
    for (i = 0; i < s_page_count; i++)
        fprintf(stderr, "  [%s] %u reads, %u writes, %u serviced, %u undecoded\n",
                s_pages[i].name, s_pages[i].reads, s_pages[i].writes,
                s_pages[i].serviced, s_pages[i].undecoded);
    fflush(stderr);
}
