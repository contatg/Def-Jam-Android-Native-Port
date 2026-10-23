/*
 * ohci.c -- OHCI 1.0a host controller registers for the MCPX.
 *
 * See ohci.h for what this is and is not. The short version: enough for a
 * title's own USB driver to find a controller and a populated root hub port,
 * plus a trace of every register access, because what the driver does after
 * that decides how the rest gets built.
 */
#include "ohci.h"
#include "../platform/mmio_decode.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/kernel.h"
#include "usb_gamepad.h"
#include "usb_hub.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* Physical addresses map to the contiguous window, which is where this
 * runtime puts physical RAM. kernel.h defines the same constant, but that
 * header pulls in the whole kernel interface for one number. */
#ifndef XBOX_CONTIG_BASE
#define XBOX_CONTIG_BASE 0x80000000u
#endif
#ifndef XBOX_CONTIG_SIZE
#define XBOX_CONTIG_SIZE (64u * 1024u * 1024u)
#endif

/* The runtime maps guest memory at a fixed host offset. */
extern ptrdiff_t xbox_GetMemoryOffset(void);

/* Calling the title's interrupt service routine.
 *
 * A recompiled function reads its arguments off the guest stack and keeps its
 * registers in thread-local storage, so it can only be called from a thread
 * that has both. xbox_worker_stack_alloc hands out a guest stack slice for
 * exactly this -- a host thread calling recompiled code -- and recomp_lookup
 * turns a guest address into something callable.
 */
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int  xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);

/* Declared in the generated runtime; thread-local, so the values below are
 * this thread's and not the guest thread's. */
#if defined(_MSC_VER)
#  define OHCI_TLS __declspec(thread)
#else
#  define OHCI_TLS __thread
#endif
extern OHCI_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern OHCI_TLS uint32_t g_ebx, g_esi, g_edi;
extern OHCI_TLS uint32_t g_fs_base;
extern uint32_t xbox_AllocThreadTib(void);

/* The vector XPP takes for USB0. HalGetInterruptVector(1) returns 1 here, and
 * the bus interrupt level is what the XDK passes. */
#define OHCI_VECTOR  1

/* ---- OHCI 1.0a operational registers, by byte offset ------------------- */
#define HcRevision              0x00
#define HcControl               0x04
#define HcCommandStatus         0x08
#define HcInterruptStatus       0x0C
#define HcInterruptEnable       0x10
#define HcInterruptDisable      0x14
#define HcHCCA                  0x18
#define HcPeriodCurrentED       0x1C
#define HcControlHeadED         0x20
#define HcControlCurrentED      0x24
#define HcBulkHeadED            0x28
#define HcBulkCurrentED         0x2C
#define HcDoneHead              0x30
#define HcFmInterval            0x34
#define HcFmRemaining           0x38
#define HcFmNumber              0x3C
#define HcPeriodicStart         0x40
#define HcLSThreshold           0x44
#define HcRhDescriptorA         0x48
#define HcRhDescriptorB         0x4C
#define HcRhStatus              0x50
#define HcRhPortStatus1         0x54
/* Through port 4: the root hub reports four downstream ports. */
#define OHCI_REG_MAX            0x64

/* HcCommandStatus */
#define CS_HCR                  0x00000001u   /* host controller reset       */

/* HcInterruptStatus / Enable */
#define INTR_SO                 0x00000001u   /* scheduling overrun          */
#define INTR_WDH                0x00000002u   /* writeback done head         */
#define INTR_SF                 0x00000004u   /* start of frame              */
#define INTR_RD                 0x00000008u   /* resume detected             */
#define INTR_UE                 0x00000010u   /* unrecoverable error         */
#define INTR_FNO                0x00000020u   /* frame number overflow       */
#define INTR_RHSC               0x00000040u   /* root hub status change      */

/* HccaFrameNumber is at +0x80 and the done head at +0x84. Declared up here
 * because the register write handler retires the done list on a WDH ack. */
#define HCCA_DONE_HEAD 0x84
static void wr32(uint32_t va, uint32_t v);
#define INTR_MIE                0x80000000u   /* master interrupt enable     */

/* HcRhPortStatus */
#define PORT_CCS                0x00000001u   /* current connect status      */
#define PORT_PES                0x00000002u   /* port enable status          */
#define PORT_PSS                0x00000004u   /* port suspend status         */
#define PORT_PPS                0x00000100u   /* port power status           */
#define PORT_LSDA               0x00000200u   /* low speed device attached   */
#define PORT_CSC                0x00010000u   /* connect status change       */
#define PORT_PESC               0x00020000u   /* enable status change        */
#define PORT_PRSC               0x00100000u   /* reset status change         */

/* Writes to HcRhPortStatus set/clear by bit position rather than by value. */
#define PORT_W_CCS_CLEAR_ENABLE 0x00000001u   /* ClearPortEnable             */
#define PORT_W_PES_SET_ENABLE   0x00000002u   /* SetPortEnable               */
#define PORT_W_PRS_SET_RESET    0x00000010u   /* SetPortReset                */
#define PORT_W_PPS_SET_POWER    0x00000100u   /* SetPortPower                */
#define PORT_W_CLEAR_POWER      0x00000200u   /* ClearPortPower              */

/* Four root hub ports, the console's four front sockets. They are not wired
 * in order: XAPI numbers the root ports 3, 4, 1, 2 as game ports 1-4 (xemu
 * attaches its controllers the same way). A pad on root port 1 is player 3 --
 * which is what Def Jam made of ours: its key events carried controller 2
 * (sub_0005F9B0 passes slot+2, Key.getController, sub_0016F480, takes 2
 * off), and the P1-P4 join screen wanted P1. */
#define OHCI_PORTS              4
static const unsigned s_game_port_to_root[4] = { 2, 3, 0, 1 };   /* 0-based */

/* The root port (0-based) the hub and pad are on: game port 1, or
 * RECOMP_PAD_PORT=1..4. */
static unsigned pad_root_port(void)
{
    static int port = -1;
    if (port < 0) {
        const char *e = getenv("RECOMP_PAD_PORT");
        int g = (e && *e) ? atoi(e) : 1;
        if (g < 1 || g > 4)
            g = 1;
        port = (int)s_game_port_to_root[g - 1];
    }
    return (unsigned)port;
}

/* Passes this thread will keep an interrupt back while the guest is at
 * DISPATCH_LEVEL or above. One pass is the OHCI_TICK_MS (4 ms) loop tick, so
 * this is two seconds.
 *
 * It was 80 ms, on the reasoning that no real critical section lasts longer.
 * True of the hardware and false here: recompiled code under an emulated
 * kernel is slower than the console by a wide and variable margin, and the
 * section XAPI holds while it opens the gamepad's interrupt pipe overran it.
 * The interrupt then landed in the middle of that setup and the title faulted
 * on a half-built structure. Two seconds is still not forever -- a guest that
 * genuinely never lowers is still not allowed to switch the device off -- but
 * it is long enough that a slow critical section is not mistaken for one. */
#define OHCI_IRQ_HOLDOFF_MS     2000u

/* Milliseconds per pass of the controller thread, which is also how many USB
 * frames a pass is worth.
 *
 * It was 20. A control transfer needs a pass per stage and enumeration needs
 * several transfers, so twenty milliseconds put the whole sequence in the
 * hundreds of milliseconds and left it racing the driver's own timeouts: the
 * same build would enumerate fully on one run and stop half way on the next,
 * with nothing to tell the two apart but how many register reads happened.
 * Four is close enough to a real frame to stop losing that race and still
 * cheap -- this thread does nothing but read a few guest dwords per pass. */
#define OHCI_TICK_MS            4u

typedef struct {
    uint32_t base;                      /* Xbox VA of the register block   */
    uint32_t reg[OHCI_REG_MAX / 4];
    unsigned reads, writes, decode_fail;
    int      index;
    int      periodic_seen;
    int      ple_seen;
    /* Bumped every time the driver writes HcInterruptStatus. The interrupt
     * thread watches it to tell a source nobody is servicing from one that
     * is simply busy -- see the delivery loop. */
    volatile unsigned ack_seq;
} OhciController;

static OhciController s_hc[2];
/* Which controller carries the device, and so which one this thread services.
 * XAPI numbers its four gamepad slots across both controllers, and which end
 * it starts from decides whether a pad shows up as player 1 or player 3 --
 * a mapping worth finding by measurement, not by assertion. */
static int s_device_hc;
/* Downstream ports the root hub reports in HcRhDescriptorA. Two is what the
 * MCPX's own hubs have; RECOMP_USB_NDP exists because which slot XAPI gives a
 * pad is decided somewhere in here and the mapping is worth measuring. */
static unsigned s_ndp = OHCI_PORTS;
static volatile int s_npads = 1; /* RECOMP_USB_PADS; raised at run time by xbox_UsbSetPadCount */
static int s_enabled;
static int s_trace;
static uint64_t s_last_write_rip;

/* MMIO acknowledgements and the bus worker share controller state. */
static INIT_ONCE s_bus_once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION s_bus_cs;
static BOOL CALLBACK bus_init(PINIT_ONCE o, PVOID p, PVOID *c)
{
    (void)o; (void)p; (void)c; InitializeCriticalSection(&s_bus_cs); return TRUE;
}
static void bus_lock(void)
{
    InitOnceExecuteOnce(&s_bus_once, bus_init, NULL, NULL);
    EnterCriticalSection(&s_bus_cs);
}
static void bus_unlock(void) { LeaveCriticalSection(&s_bus_cs); }

static uint32_t *reg_of(OhciController *hc, uint32_t off)
{
    return (off < OHCI_REG_MAX) ? &hc->reg[off / 4] : NULL;
}

/* ---- register semantics ------------------------------------------------ */

static uint32_t ohci_frame_now(void);
static uint64_t ohci_clock(uint32_t interval, uint32_t *remaining);
static void usb_reset_device(int dev);

static uint64_t ohci_read_locked(void *dev, uint32_t off, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = r ? *r : 0;

    hc->reads++;

    /* HcFmRemaining and HcFmNumber advance on their own. A driver that waits
     * for the frame counter to move is waiting for the controller to be
     * running, and a counter that never changes is a controller that is not.
     * Host milliseconds, the same clock the HCCA frame number uses
     * (ohci_frame_now, below). */
    if (aligned == HcFmNumber) {
        v = ohci_frame_now();
    } else if (aligned == HcFmRemaining) {
        /* Where we are inside the current frame, counting down from the frame
         * interval as the hardware does. From the same clock as the frame
         * number, so the two agree rather than being unrelated. */
        uint32_t interval = hc->reg[HcFmInterval / 4] & 0x3FFFu;
        if (!interval)
            interval = 0x2EDF;                 /* 11999, the standard value */
        ohci_clock(interval, &v);
    }

    /* Sub-dword reads take their slice of the containing register. */
    if (size < 4) {
        unsigned shift = (off & 3u) * 8u;
        v >>= shift;
        if (size == 1) v &= 0xFFu;
        else if (size == 2) v &= 0xFFFFu;
    }

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] read  +0x%02X = %08X\n",
                    hc->index, off, (uint32_t)v);
    }
    return v;
}

static void ohci_write_locked(void *dev, uint32_t off, uint64_t val, int size)
{
    OhciController *hc = (OhciController *)dev;
    uint32_t aligned = off & ~3u;
    uint32_t *r = reg_of(hc, aligned);
    uint32_t v = (uint32_t)val;

    hc->writes++;
    if (!r)
        return;

    if (s_trace) {
        static unsigned n;
        if (n++ < 600)
            fprintf(stderr, "  [OHCI%d] write +0x%02X = %08X\n",
                    hc->index, off, v);
    }

    /* The list pointers are worth naming their writer for even when the
     * general trace is off: a bad one stops enumeration dead and says
     * nothing, and the guest instruction that wrote it is the whole lead.
     * Gated so an ordinary run stays quiet. */
    if (s_trace && (aligned == HcControlHeadED || aligned == HcBulkHeadED
                 || aligned == HcHCCA
                 || (aligned >= HcRhPortStatus1 && aligned < OHCI_REG_MAX))) {
        fprintf(stderr, "  [OHCI%d] reg +0x%02X = %08X from RIP 0x%llX\n",
                hc->index, aligned, v,
                (unsigned long long)s_last_write_rip);
        fflush(stderr);
    }

    switch (aligned) {
    case HcRevision:                    /* read-only */
        return;

    case HcCommandStatus:
        /* HCR is self-clearing: the controller resets and drops the bit, and
         * a driver polls for exactly that. Leaving it set is a hang, and it
         * is the first thing a driver does, so it would be the only thing
         * anyone ever saw of this file. */
        *r |= v;
        if (*r & CS_HCR) {
            *r &= ~CS_HCR;
            hc->reg[HcControl / 4] &= ~0xC0u;    /* back to UsbReset state  */
            hc->reg[HcInterruptStatus / 4] = 0;
            hc->reg[HcInterruptEnable / 4] = 0;
        }
        return;

    case HcInterruptStatus:
        /* Acknowledging WritebackDoneHead retires the done list.
         *
         * The controller publishes the queue in HccaDoneHead and raises WDH;
         * the driver reads the list and clears WDH to say it is finished, and
         * the head must then read as zero. Leaving the old value there is a
         * stale pointer to descriptors that have already been consumed, and a
         * driver that checks the head before trusting the interrupt walks
         * them a second time. That is a plausible way to enumerate a device
         * perfectly and then stop, which is what happened here. */
        if ((v & INTR_WDH) && (*r & INTR_WDH)) {
            uint32_t hcca = hc->reg[HcHCCA / 4];
            if (hcca)
                wr32(hcca + HCCA_DONE_HEAD, 0);
            hc->reg[HcDoneHead / 4] = 0;
        }
        *r &= ~v;                       /* write 1 to clear                 */
        hc->ack_seq++;
        return;

    case HcInterruptEnable:
        hc->reg[HcInterruptEnable / 4] |= v;
        return;

    case HcInterruptDisable:
        hc->reg[HcInterruptEnable / 4] &= ~v;
        return;

    case HcFmNumber:
    case HcFmRemaining:
        return;                         /* driven by the controller         */

    case HcRhDescriptorA:
        /* NumberDownstreamPorts is ours; the driver may set the power and
         * over-current policy bits above it. */
        *r = (*r & 0x000000FFu) | (v & ~0x000000FFu);
        return;

    case HcRhPortStatus1:
    case HcRhPortStatus1 + 4:
    case HcRhPortStatus1 + 8:
    case HcRhPortStatus1 + 12: {
        /* Writes here are set/clear requests by bit position, not a value to
         * store. Getting that wrong looks like a port that will not enable.
         *
         * All four, not the two this root hub usually advertises. The read
         * path already answers four, because a driver reads every port
         * register whatever the descriptor says; the write path did not, so a
         * device placed on port 3 was seen, reset, and never reset-completed
         * -- the request fell through to the plain store and PRSC was never
         * raised. The driver reset it forever. A hub that reports N ports has
         * to give all N the same semantics. */
        unsigned port = (aligned - HcRhPortStatus1) / 4;
        uint32_t *ps = &hc->reg[(HcRhPortStatus1 + port * 4) / 4];

        /* Write-1-to-clear first, because the change bits a write carries
         * refer to the state BEFORE it.
         *
         * This ordering is the whole bug it once had. A driver starting a
         * fresh reset writes SetPortReset and ClearPortResetStatusChange in
         * the same word -- "acknowledge the last reset, begin another" --
         * which is both legal and what the Xbox USB stack does: DDS9 writes
         * 0x01100010. Clearing afterwards wiped the PRSC this very write had
         * just raised, so the reset never reported complete, the driver
         * reset the port again, and enumeration looped forever one step from
         * finishing. The port looked healthy the whole time: connected,
         * enabled, powered. */
        *ps &= ~(v & 0xFFFF0000u);

        if (v & PORT_W_CCS_CLEAR_ENABLE) *ps &= ~PORT_PES;
        if (v & PORT_W_PES_SET_ENABLE)   *ps |= (*ps & PORT_CCS) ? PORT_PES : 0;
        if (v & PORT_W_PPS_SET_POWER)    *ps |= PORT_PPS;
        if (v & PORT_W_CLEAR_POWER)      *ps &= ~PORT_PPS;
        if (v & PORT_W_PRS_SET_RESET) {
            /* Reset completes immediately -- there is no wire to settle -- so
             * PRS is never observed set. What matters is what the driver
             * checks afterwards: a present device comes back enabled, and the
             * reset-change bit says the reset finished.
             *
             * Only the status bit is set here. Delivering the interrupt is the
             * controller thread's job, because this runs on the guest's own
             * thread inside a fault handler, and pointing g_esp at a worker
             * stack from here would overwrite the stack pointer of the thread
             * being interrupted. */
            if (*ps & PORT_CCS)
                *ps |= PORT_PES;
            *ps |= PORT_PRSC;
            if (hc->index == s_device_hc) {
                unsigned pad;
                for (pad = 0; pad < (unsigned)s_npads; pad++) {
                    if (port != (pad_root_port() + pad) % OHCI_PORTS) continue;
                    if (pad == 0) {
                        usb_hub_reset();
                        usb_reset_device(0);
                    } else {
                        usb_gamepad_reset((int)pad);
                    }
                    usb_reset_device((int)pad + 1);
                }
            }
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
        }
        return;
    }

    default:
        break;
    }

    if (size == 4) {
        *r = v;
    } else {
        unsigned shift = (off & 3u) * 8u;
        uint32_t mask = ((size == 1) ? 0xFFu : 0xFFFFu) << shift;
        *r = (*r & ~mask) | ((v << shift) & mask);
    }
}

static uint64_t ohci_read(void *dev, uint32_t off, int size)
{
    uint64_t value;
    bus_lock(); value = ohci_read_locked(dev, off, size); bus_unlock();
    return value;
}
static void ohci_write(void *dev, uint32_t off, uint64_t value, int size)
{
    bus_lock(); ohci_write_locked(dev, off, value, size); bus_unlock();
}

/* ---- the control list -------------------------------------------------- */
/*
 * A host controller is a bus master: the driver builds endpoint and transfer
 * descriptors in RAM, points HcControlHeadED at them and sets ControlListFilled,
 * and the controller walks that list itself. Nothing arrives through MMIO, so
 * none of this is visible to the register trace -- which is why the driver
 * looked idle after the port came up.
 *
 * OHCI 1.0a, section 4. An endpoint descriptor is four dwords:
 *
 *   +0  FA | EN<<7 | D<<11 | S<<13 | K<<14 | F<<15 | MPS<<16
 *   +4  TailP        queue tail, 16-byte aligned
 *   +8  HeadP        queue head, with Halted in bit 0 and toggleCarry in bit 1
 *   +C  NextED
 *
 * and a general transfer descriptor is four more:
 *
 *   +0  ... DP<<19 | DI<<21 | T<<24 | EC<<26 | CC<<28
 *   +4  CBP          current buffer pointer, 0 when the transfer moved nothing
 *   +8  NextTD
 *   +C  BE           last byte of the buffer, inclusive
 *
 * A transfer is done when HeadP reaches TailP. Completed descriptors go on the
 * done queue, newest first, and the controller publishes it in the HCCA and
 * raises WritebackDoneHead.
 */
#define ED_SKIP        (1u << 14)
#define ED_HEAD_HALT   (1u << 0)
#define ED_HEAD_TOGGLE (1u << 1)
#define ED_PTR_MASK    0xFFFFFFF0u

#define TD_DP_SETUP    0u
#define TD_DP_OUT      1u
#define TD_DP_IN       2u
#define TD_CC_NOERROR  0u
#define TD_CC_STALL    4u
#define TD_CC_DATAUNDERRUN 9u
#define TD_ROUNDING    (1u << 18)   /* bufferRounding: a short packet is fine */

#define HCCA_FRAME_NO 0x80
#define USB_DEV_HUB 0
#define USB_DEV_PAD(pad) ((pad) + 1)
#define USB_DEV_COUNT (USB_GAMEPAD_MAX + 1)
#define TD_CC_NOT_RESPONDING 5u
static int s_plugged_pads;
static uint32_t s_reset_seen[USB_GAMEPAD_MAX];
static uint32_t g_setup_pending_[USB_DEV_COUNT];
static UsbSetup g_setup_[USB_DEV_COUNT];
static uint8_t g_ctrl_buf_[USB_DEV_COUNT][256];
static int g_ctrl_len_[USB_DEV_COUNT] = {-1,-1,-1,-1,-1};
static int g_ctrl_sent_[USB_DEV_COUNT];
#define g_setup_pending g_setup_pending_[dev]
#define g_setup g_setup_[dev]
#define g_ctrl_buf g_ctrl_buf_[dev]
#define g_ctrl_len g_ctrl_len_[dev]
#define g_ctrl_sent g_ctrl_sent_[dev]

/* QPC supplies both whole USB milliseconds and the phase within one frame. */
static uint64_t ohci_clock(uint32_t interval, uint32_t *remaining)
{
    LARGE_INTEGER q, f;
    uint64_t scaled, millis, phase;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    scaled = (uint64_t)q.QuadPart * 1000u;
    millis = scaled / (uint64_t)f.QuadPart;
    phase = scaled % (uint64_t)f.QuadPart;
    if (remaining)
        *remaining = interval - (uint32_t)(phase * interval / (uint64_t)f.QuadPart);
    return millis;
}
static uint32_t ohci_frame_now(void) { return (uint32_t)ohci_clock(0, NULL) & 0xFFFFu; }

static int usb_route(uint32_t fa)
{
    int pad;
    if (!s_plugged_pads) return -1;
    if (fa) {
        if (fa == usb_hub_address()) return USB_DEV_HUB;
        for (pad = 0; pad < s_plugged_pads; pad++)
            if ((pad != 0 || usb_hub_pad_enabled()) && fa == usb_gamepad_address(pad))
                return USB_DEV_PAD(pad);
        return -1;
    }
    if (!usb_hub_address()) return USB_DEV_HUB;
    if (usb_hub_pad_enabled() && !usb_gamepad_address(0)) return USB_DEV_PAD(0);
    for (pad = 1; pad < s_plugged_pads; pad++)
        if (!usb_gamepad_address(pad)) return USB_DEV_PAD(pad);
    return -1;
}
static int usb_dev_control(int dev, const UsbSetup *setup, uint8_t *out, int max)
{
    return dev == USB_DEV_HUB ? usb_hub_control(setup, out, max)
        : usb_gamepad_control(dev - 1, setup, out, max);
}
static uint8_t s_int_buf[32];
static int s_int_len;


/* Every address below came out of guest memory, so none of them are trusted.
 *
 * This is not theoretical. The driver writes HcControlHeadED twice during
 * bring-up, and the second write is 0xCCCCCCCC -- MSVC's uninitialised-memory
 * fill, left there by a local that was never assigned. Following it lands far
 * outside the mapping and takes the runtime down with an access violation,
 * which is a crash the title itself would never have had. A device model
 * following a pointer a title left lying around has to check it first.
 */
/* Two windows are legal here. The low one is the ordinary guest mapping;
 * the contiguous one at CONTIG_BASE is where this runtime puts physical
 * RAM, and it is where every descriptor the controller follows actually
 * lives. Checking only the low window rejected all of them, which is why
 * the control list was never walked. */
static int guest_ok(uint32_t va, uint32_t bytes)
{
    size_t mapped = xbox_GetMappedSize();

    if (va == 0)
        return 0;
    if (va >= XBOX_CONTIG_BASE)
        return (uint64_t)va + bytes
            <= (uint64_t)XBOX_CONTIG_BASE + XBOX_CONTIG_SIZE;
    return mapped != 0 && (uint64_t)va + bytes <= (uint64_t)mapped;
}

/*
 * Every pointer the controller follows is a PHYSICAL address.
 *
 * That is what OHCI specifies and what the driver puts in the registers: it
 * allocates its descriptors out of contiguous memory and writes
 * MmGetPhysicalAddress of them, which this runtime correctly returns as the
 * virtual address minus the contiguous window's base. So a list head arrives
 * here as something like 0x000009A0, and dereferencing that as a guest virtual
 * address reads the title's own image instead of an endpoint descriptor.
 *
 * Translating back is the whole of it: physical P is virtual CONTIG_BASE + P.
 * An address already inside that window is passed through, because this driver
 * writes one by mistake during bring-up and landing somewhere real beats being
 * rejected.
 *
 * Without this the control list was never walked. The port reset completed,
 * the driver queued its first SETUP and set ControlListFilled, and the
 * descriptor sat in memory for ever: no device was ever addressed, no
 * descriptors were ever read, and the title saw no controller.
 */
/* Ask the kernel what it handed out before falling back to arithmetic.
 *
 * The rule below -- physical addresses live in the contiguous window -- holds
 * for memory a driver allocated for DMA, and that is most of what arrives
 * here: head pointers, the HCCA, descriptors. It does not hold for a buffer
 * that is simply part of the title's data, whose physical address the kernel
 * reports as the identity, and this title's USB driver reads its device
 * descriptor into exactly such a buffer. Applying the rule to it put every
 * descriptor 0x80000000 bytes from the driver waiting to read it, so the
 * driver saw an empty buffer, rejected the device, reset the port, and tried
 * again for as long as it was willing to.
 *
 * xbox_PhysicalToVirtual answers from the translations the kernel actually
 * performed, so it is right by construction where it answers at all, and
 * returns zero rather than a guess where it does not. */
extern uint32_t xbox_PhysicalToVirtual(uint32_t pa);

static uint32_t phys_to_guest(uint32_t pa)
{
    uint32_t va = xbox_PhysicalToVirtual(pa);
    if (va)
        return va;
    return (pa >= XBOX_CONTIG_BASE) ? pa : XBOX_CONTIG_BASE + pa;
}

static uint32_t rd32(uint32_t pa)
{
    uint32_t va = phys_to_guest(pa);
    if (!guest_ok(va, 4))
        return 0;
    return *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}
static void wr32(uint32_t pa, uint32_t v)
{
    uint32_t va = phys_to_guest(pa);
    if (!guest_ok(va, 4))
        return;
    *(uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va) = v;
}
static uint8_t *guest_ptr(uint32_t pa, uint32_t bytes)
{
    uint32_t va = phys_to_guest(pa);
    return guest_ok(va, bytes)
         ? (uint8_t *)xbox_GetMemoryOffset() + va : NULL;
}

/* Transfer counts for RECOMP_USB_STATS: input reports served, control
 * transfer stages, and OUT packets (rumble) by endpoint. */
static unsigned long s_n_report, s_n_ctrl, s_n_out_ep0, s_n_out_other;

/* Move one transfer descriptor. Returns the condition code to report. */
static uint32_t ohci_do_td(OhciController *hc, uint32_t ed0, uint32_t td)
{
    uint32_t info = rd32(td);
    uint32_t cbp  = rd32(td + 4);
    uint32_t be   = rd32(td + 12);
    uint32_t dp   = (info >> 19) & 3u;
    int      len  = (cbp && be >= cbp) ? (int)(be - cbp + 1) : 0;
    uint32_t endpoint = (ed0 >> 7) & 0xFu;
    int      moved = 0;
    int      dev = usb_route(ed0 & 0x7Fu);

    if (dev > 0 && s_reset_seen[dev - 1] != usb_gamepad_reset_generation(dev - 1))
        usb_reset_device(dev);
    if (dev < 0)
        return TD_CC_NOT_RESPONDING;     /* nothing at that address */


    if (s_trace) {
        static unsigned n;
        if (n++ < 80) {
            fprintf(stderr, "  [OHCI%d] TD %08X dp=%u ep=%u cbp=%08X be=%08X "
                            "len=%d\n", hc->index, td, dp, endpoint, cbp, be,
                    len);
            fflush(stderr);
        }
    }
    if (dp == TD_DP_SETUP) {
        /* Eight bytes of setup, kept for the data stage that follows. */
        s_n_ctrl++;
        if (len >= 8) {
            const uint8_t *p = guest_ptr(cbp, 8);
            if (!p) return TD_CC_NOERROR;
            g_setup.bmRequestType = p[0];
            g_setup.bRequest      = p[1];
            g_setup.wValue        = (uint16_t)(p[2] | (p[3] << 8));
            g_setup.wIndex        = (uint16_t)(p[4] | (p[5] << 8));
            g_setup.wLength       = (uint16_t)(p[6] | (p[7] << 8));
            g_setup_pending = 1;
            g_ctrl_len = -1;   /* answered lazily on the first IN */
            g_ctrl_sent = 0;
            if (s_trace) {
                fprintf(stderr, "  [OHCI%d] SETUP %02X %02X value %04X "
                                "index %04X len %u\n",
                        hc->index, g_setup.bmRequestType, g_setup.bRequest,
                        g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                fflush(stderr);
            }
            moved = 8;
        }
    } else if (dp == TD_DP_IN) {
        if (endpoint == 0) {
            /* Data stage of a control transfer, or its status stage when the
             * driver asks for nothing. */
            int n;

            if (!g_setup_pending)
                return TD_CC_NOERROR;
            if (g_ctrl_len < 0) {
                g_ctrl_len = usb_dev_control(dev, &g_setup, g_ctrl_buf,
                                                 (int)sizeof g_ctrl_buf);
                if (g_ctrl_len < 0) {
                    /* Worth saying out loud. A stall here halts the
                     * endpoint until the driver clears it, and a driver
                     * that sees one usually stops using the device -- so
                     * an unhandled request is not a gap that degrades
                     * gracefully, it is one that ends input. */
                    fprintf(stderr, "  [OHCI%d] STALL: unhandled control "
                            "request %02X %02X value %04X index %04X len %u\n",
                            hc->index, g_setup.bmRequestType, g_setup.bRequest,
                            g_setup.wValue, g_setup.wIndex, g_setup.wLength);
                    fflush(stderr);
                    g_setup_pending = 0;
                    return TD_CC_STALL;
                }
                g_ctrl_sent = 0;
            }
            /* Whatever is left, capped by this descriptor's buffer. A short
             * packet is how the device says "that is all", so running out is
             * the normal end of the stage rather than an error. */
            n = g_ctrl_len - g_ctrl_sent;
            if (n > len) n = len;
            if (n < 0) n = 0;
            if (n > 0 && !guest_ptr(cbp, (uint32_t)n)) return 12u; /* buffer overrun */
            if (n > 0)
                memcpy(guest_ptr(cbp, (uint32_t)n),
                       g_ctrl_buf + g_ctrl_sent, (size_t)n);
            g_ctrl_sent += n;
            moved = n;
            if (s_trace && n > 0) {
                fprintf(stderr, "  [OHCI%d] IN ep0 %d bytes (%d/%d)\n",
                        hc->index, n, g_ctrl_sent, g_ctrl_len);
                fflush(stderr);
            }
        } else {
            /* An interrupt endpoint: the hub's status change or the pad's
             * report, prepared by the list walker (which also decided it was
             * worth completing rather than NAKing). */
            int n = s_int_len;
            if (n > len) n = len;
            if (n > 0 && !guest_ptr(cbp, (uint32_t)n)) return 12u; /* buffer overrun */
            if (n > 0)
                memcpy(guest_ptr(cbp, (uint32_t)n), s_int_buf, (size_t)n);
            moved = n;
        }
    } else {
        /* OUT: the status stage of an IN control transfer, or rumble. Both
         * are accepted and discarded -- but only the control pipe's ends a
         * control transfer. A rumble packet on endpoint 2 used to clear the
         * pending setup too, so a force-feedback write landing between a
         * control request's stages broke that request; Burnout 3 starts its
         * engine-rev rumble on the Crash countdown and then paused with the
         * pad unresponsive. */
        moved = len;
        if (endpoint == 0) {
            /* A class SET_REPORT's data stage is an OUT with bytes in it. The
             * only such report a pad takes is rumble. */
            if (dev > 0 && len >= 6 && g_setup_pending && g_setup.bmRequestType == 0x21
                    && g_setup.bRequest == 0x09) {
                const uint8_t *p = guest_ptr(cbp, (uint32_t)len);
                if (p) usb_gamepad_out(dev - 1, p, len);
            }
            g_setup_pending = 0;
            s_n_out_ep0++;
        } else {
            /* Rumble on the pad's interrupt OUT endpoint. */
            if (dev > 0 && len > 0) {
                const uint8_t *p = guest_ptr(cbp, (uint32_t)len);
                if (p) usb_gamepad_out(dev - 1, p, len);
            }
            s_n_out_other++;
        }
    }

    /* CBP is zero when everything asked for moved, and otherwise points past
     * what did. A driver computes the transferred length from it. */
    wr32(td + 4, (moved >= len) ? 0u : cbp + (uint32_t)moved);
    /* Every descriptor, not just the ones with their own message. A control
     * transfer is three of these and the interesting question is usually which
     * of the three is missing. */
    if (s_trace) {
        static unsigned n;
        if (n++ < 60) {
            /* The buffer address as well as the length. Where the bytes
             * landed is what a watchpoint needs, and deriving it afterwards
             * from a dump of the descriptor words invites reading a physical
             * address as a guest one -- which is the mistake this whole file
             * exists to stop making. CBP is physical and reads back as zero
             * once everything asked for has moved, so the value printed is
             * the one the transfer started with. */
            /* Read the bytes back out of guest memory rather than trusting
             * the count. "moved" comes from the device model and says what it
             * produced, not what reached the buffer: the copy is skipped
             * silently when the address does not pass guest_ok, and the two
             * cases are indistinguishable from the count alone. A driver that
             * rejects a device looks the same whether the descriptor was
             * wrong or was never delivered. */
            const uint8_t *seen = (cbp && moved > 0)
                                ? guest_ptr(cbp, (uint32_t)moved) : NULL;
            fprintf(stderr, "  [OHCI%d] TD 0x%08X %-5s ep%u asked %d moved %d "
                            "buf phys 0x%08X (guest 0x%08X)%s%02X %02X %02X "
                            "%02X %02X %02X %02X %02X\n",
                    hc->index, td,
                    dp == TD_DP_SETUP ? "SETUP" : dp == TD_DP_IN ? "IN" : "OUT",
                    endpoint, len, moved, cbp,
                    cbp ? phys_to_guest(cbp) : 0,
                    seen ? " in memory: " : " NOT IN MEMORY ",
                    seen && moved > 0 ? seen[0] : 0,
                    seen && moved > 1 ? seen[1] : 0,
                    seen && moved > 2 ? seen[2] : 0,
                    seen && moved > 3 ? seen[3] : 0,
                    seen && moved > 4 ? seen[4] : 0,
                    seen && moved > 5 ? seen[5] : 0,
                    seen && moved > 6 ? seen[6] : 0,
                    seen && moved > 7 ? seen[7] : 0);
        }
        fflush(stderr);
    }

    /* A short IN packet on a TD without bufferRounding is DATA UNDERRUN
     * (OHCI 1.0a): the controller retires this TD with that code
     * and halts the endpoint -- the walker does so for any non-zero code --
     * leaving the rest of the transfer for the driver to retire. A short
     * packet still ends the stage; what the driver needs to see is that it
     * did. Reporting NOERROR, and completing the trailing TDs with zero
     * bytes, sent XAPI's done-queue handler down its success path, which
     * walks from the ED head into the dummy tail TD and follows its garbage
     * link: X-Men Legends asks for 80 bytes of configuration descriptor,
     * gets 32, and faulted there. */
    if (dp == TD_DP_IN && moved < len && !(info & TD_ROUNDING))
        return TD_CC_DATAUNDERRUN;
    return TD_CC_NOERROR;
}

/* The pad's report as last delivered, so the periodic list can answer NAK
 * (leave the descriptor queued) while nothing has changed, as a real pad does.
 * Completing one every poll would be legal and would cost the title an
 * interrupt and a deferred call every four milliseconds for nothing. */
static uint8_t s_last_report[USB_GAMEPAD_MAX][32];
static unsigned s_pad_polls[USB_GAMEPAD_MAX];              /* times the pad's IN endpoint was asked */
static volatile unsigned s_loop_passes, s_loop_masked;  /* controller passes; ones with MIE off */
static ULONGLONG s_masked_since;          /* when MIE went off, 0 while it is on */
static int s_report_sent[USB_GAMEPAD_MAX];

static void usb_reset_device(int dev)
{
    if (dev < 0 || dev >= USB_DEV_COUNT) return;
    g_setup_pending_[dev] = 0;
    g_ctrl_len_[dev] = -1;
    g_ctrl_sent_[dev] = 0;
    if (dev > 0) {
        s_report_sent[dev - 1] = 0;
        s_reset_seen[dev - 1] = usb_gamepad_reset_generation(dev - 1);
    }
}

/* Walk one endpoint list from its head. The done queue is threaded through
 * *done_head, newest first, so the control and periodic lists can share it.
 * Returns how many descriptors completed. */
static int ohci_run_list(OhciController *hc, uint32_t head_pa, int periodic,
                         uint32_t *done_head_io)
{
    uint32_t ed = head_pa & ED_PTR_MASK;
    uint32_t done_head = *done_head_io;
    int completed = 0, guard = 0;

    while (ed && guest_ok(phys_to_guest(ed), 16) && ++guard < 64) {
        uint32_t ed0  = rd32(ed);
        uint32_t tail = rd32(ed + 4) & ED_PTR_MASK;
        uint32_t head = rd32(ed + 8);
        int tguard = 0;

        /* An endpoint that is skipped, halted or empty looks identical from
         * outside: the list is walked and nothing moves. Saying which it is
         * turns 'enumeration stopped' into a specific question. */
        if (s_trace) {
            /* Only when something about this endpoint changed.
             *
             * The list is walked every millisecond, so a flat budget of lines
             * is spent during start-up by endpoints sitting idle, and is gone
             * long before the interesting part. That has already cost one
             * wrong reading here -- an empty control list was reported as the
             * state after a transfer when it was really the state before one,
             * because the trace had stopped printing. An endpoint that has
             * not moved has nothing to say, and one that has is worth a line
             * however late it happens. */
            static struct { uint32_t ed, head, tail, ctl; } seen[16];
            static unsigned nseen;
            unsigned i;

            for (i = 0; i < nseen; i++)
                if (seen[i].ed == ed)
                    break;
            if (i == nseen && nseen < 16) {
                seen[nseen].ed = ed;
                seen[nseen].head = ~head;   /* force the first report */
                nseen++;
            }
            if (i < 16 && (seen[i].head != head || seen[i].tail != tail
                        || seen[i].ctl != ed0)) {
                seen[i].head = head;
                seen[i].tail = tail;
                seen[i].ctl  = ed0;
                fprintf(stderr, "  [OHCI%d] ED 0x%08X ctl %08X head %08X tail %08X%s%s\n",
                        hc->index, ed, ed0, head, tail,
                        (ed0 & ED_SKIP) ? " SKIP" : "",
                        (head & ED_HEAD_HALT) ? " HALTED" : "");
                fflush(stderr);
            }
        }
        if (ed0 & ED_SKIP) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }
        if (head & ED_HEAD_HALT) { ed = rd32(ed + 12) & ED_PTR_MASK; continue; }

        while ((head & ED_PTR_MASK) != tail
            && (head & ED_PTR_MASK) && guest_ok(phys_to_guest(head & ED_PTR_MASK), 16) && ++tguard < 64) {
            uint32_t td = head & ED_PTR_MASK;
            uint32_t next = rd32(td + 8) & ED_PTR_MASK;
            uint32_t cc;
            int report_pad = -1;

            /* An interrupt IN endpoint with nothing new: NAK, which leaves the
             * descriptor where it is for the next poll. The hub has something
             * to say only when a port changed; the pad, when its state did. */
            s_int_len = 0;
            if (((ed0 >> 7) & 0xFu) != 0
                    && ((rd32(td) >> 19) & 3u) == TD_DP_IN) {
                int dev = usb_route(ed0 & 0x7Fu);
                if (dev > 0 && s_reset_seen[dev - 1] != usb_gamepad_reset_generation(dev - 1))
                    usb_reset_device(dev);
                if (dev == USB_DEV_HUB) {
                    int n = usb_hub_status_change(s_int_buf, (int)sizeof s_int_buf);
                    if (n <= 0)
                        break;
                    s_int_len = n;
                } else if (dev >= USB_DEV_PAD(0)) {
                    int pad = dev - 1;
                    int n = usb_gamepad_report(pad, s_int_buf, (int)sizeof s_int_buf);
                    s_pad_polls[pad]++;
                    if (periodic && s_report_sent[pad] && n > 0 && !memcmp(s_int_buf, s_last_report[pad], (size_t)n))
                        break;
                    report_pad = pad;
                    {
                        /* RECOMP_PAD_REPORT_TRACE=N: the first N reports the pad
                         * sends, each with its time and the polls it went unasked,
                         * so a press the title missed can be placed: never sent,
                         * sent merged with the next, or sent and ignored. */
                        static int trace = -1;
                        static unsigned polls_at_last[USB_GAMEPAD_MAX];
                        if (trace < 0) {
                            const char *e = getenv("RECOMP_PAD_REPORT_TRACE");
                            trace = e ? atoi(e) : 0;
                        }
                        if (trace > 0) {
                            trace--;
                            fprintf(stderr, "  [PAD] report at %.3f s after %u polls: buttons %02X"
                                            " a %02X b %02X x %02X y %02X%c",
                                    (double)GetTickCount64() / 1000.0,
                                    s_pad_polls[pad] - polls_at_last[pad], s_int_buf[2], s_int_buf[4],
                                    s_int_buf[5], s_int_buf[6], s_int_buf[7], 10);
                            polls_at_last[pad] = s_pad_polls[pad];
                        }
                    }
                    s_int_len = n;
                }
            }
            cc = ohci_do_td(hc, ed0, td);
            if (cc == TD_CC_NOERROR && report_pad >= 0 && s_int_len > 0) {
                memcpy(s_last_report[report_pad], s_int_buf, (size_t)s_int_len);
                s_report_sent[report_pad] = 1;
            }

            /* Report the outcome where the driver reads it, then put the
             * descriptor on the done queue, newest first. */
            wr32(td, (rd32(td) & 0x0FFFFFFFu) | (cc << 28));
            wr32(td + 8, done_head);
            done_head = td;
            completed++;

            head = next | (head & ED_HEAD_TOGGLE);
            if (cc != TD_CC_NOERROR) {
                head |= ED_HEAD_HALT;       /* a stall halts the endpoint */
                break;
            }
        }
        wr32(ed + 8, head);
        ed = rd32(ed + 12) & ED_PTR_MASK;
    }
    *done_head_io = done_head;
    return completed;
}

/* Hand the done queue to the driver: the HCCA's done head, the register, and
 * the writeback-done-head interrupt. */
static void ohci_publish_done(OhciController *hc, uint32_t done_head)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];
    if (hcca)
        wr32(hcca + HCCA_DONE_HEAD, done_head);
    hc->reg[HcDoneHead / 4] = done_head;
    hc->reg[HcInterruptStatus / 4] |= INTR_WDH;
}

/* The periodic list for this frame: the HCCA's interrupt table has 32 heads,
 * one per frame number modulo 32, and a driver links an endpoint polled every
 * N ms into every Nth of them.
 *
 * This is where a pad's interrupt endpoint lives, so it is how input reaches a
 * title at all. Only the control list used to be walked: the pad enumerated,
 * and then never reported a button. */
static int ohci_run_periodic_frame(OhciController *hc, uint32_t frame, uint32_t *done_head)
{
    uint32_t hcca = hc->reg[HcHCCA / 4];
    uint32_t head;
    if (!hcca)
        return 0;
    head = rd32(hcca + ((frame & 31u) * 4u));
    if (!head)
        return 0;
    {
        static unsigned walks, reports;
        int n = ohci_run_list(hc, head, 1, done_head);
        walks++;
        {
            /* Every 10 s: whether the periodic list is still being walked and
             * how many pad polls and reports that came to -- the only way to
             * tell "the pad was never asked" from "the title ignored it". */
            static ULONGLONG next_ms;
            static unsigned last_walks, last_polls, last_reports;
            ULONGLONG now = GetTickCount64();
            if (!next_ms)
                next_ms = now + 10000;
            if (now >= next_ms) {
                static unsigned last_passes, last_masked;
                unsigned pad, polls = 0;
                for (pad = 0; pad < USB_GAMEPAD_MAX; pad++) polls += s_pad_polls[pad];
                fprintf(stderr, "  [OHCI%d] periodic: %u walks, %u pad polls, %u reports in the last %llu s"
                                " (%u passes, %u with interrupts masked)\n",
                        hc->index, walks - last_walks, polls - last_polls,
                        reports - last_reports, (unsigned long long)(now - next_ms + 10000) / 1000,
                        s_loop_passes - last_passes, s_loop_masked - last_masked);
                last_walks = walks; last_polls = polls; last_reports = reports;
                last_passes = s_loop_passes; last_masked = s_loop_masked;
                next_ms = now + 10000;
            }
        }
        if (n > 0 && (reports++ < 5 || reports % 100 == 0
                      || s_last_report[0][2] || s_last_report[0][4]))
            fprintf(stderr, "  [OHCI%d] periodic: %d descriptor(s) completed, report %02X %02X"
                            " %02X %02X (#%u, %u walks)\n", hc->index, n,
                    s_last_report[0][2], s_last_report[0][4], s_last_report[0][5], s_last_report[0][6],
                    reports, walks);
        return n;
    }
}

/* Each frame since the last pass gets its walk: the thread sleeps between
 * passes, and a frame it slept through is a poll the pad never had. At most
 * one lap of the table, so a long stall does not replay a backlog. */
static int ohci_run_periodic(OhciController *hc, uint32_t *done_head)
{
    static uint64_t last = UINT64_MAX;
    uint64_t now = ohci_clock(0, NULL), elapsed;
    uint32_t span, f;
    int n = 0;

    if (last == UINT64_MAX) last = now - 1u;
    elapsed = now - last;
    span = elapsed > 32u ? 32u : (uint32_t)elapsed;
    for (f = span; f > 0; f--)
        n += ohci_run_periodic_frame(hc, (now - f + 1u) & 0xFFFFu, done_head);
    last = now;
    return n;
}

/* ---- raising an interrupt --------------------------------------------- */

/* Call the title's ISR on this thread, with a guest stack under it.
 *
 * BOOLEAN ServiceRoutine(PKINTERRUPT Interrupt, PVOID ServiceContext), stdcall,
 * so the two arguments go on the stack right to left with a return address on
 * top. The sentinel is what the routine pops on the way out; nothing jumps to
 * it, and a recognisable value beats a real address if it ever shows up in a
 * report.
 *
 * Returns what the routine returned: an ISR that does not claim the interrupt
 * returns FALSE, and that is worth seeing rather than assuming.
 */
static int ohci_call_isr(OhciController *hc)
{
    uint32_t kinterrupt = xbox_GetConnectedInterrupt(OHCI_VECTOR);
    uint32_t routine, context;
    recomp_func_t fn;
    uint8_t *mem;
    int slot;

    if (!kinterrupt)
        return -1;                      /* nothing connected yet            */

    mem     = (uint8_t *)xbox_GetMemoryOffset();
    routine = *(uint32_t *)(mem + kinterrupt + 0);
    context = *(uint32_t *)(mem + kinterrupt + 4);
    if (!routine)
        return -1;

    fn = recomp_lookup(routine);
    if (!fn) {
        fprintf(stderr, "  [OHCI%d] ISR 0x%08X has no translation\n",
                hc->index, routine);
        fflush(stderr);
        return -1;
    }

    /* One slice for the life of the raise. Sixteen exist and this takes one
     * only while the routine runs, so a title using them for its own workers
     * is not starved by a controller that interrupts. */
    slot = xbox_worker_stack_alloc();
    if (slot < 0) {
        fprintf(stderr, "  [OHCI%d] no worker stack for the ISR\n", hc->index);
        fflush(stderr);
        return -1;
    }

    g_esp = XBOX_WORKER_STACK_TOP(slot);
    g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;

    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;      /* arg 2 */
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kinterrupt;   /* arg 1 */
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;  /* return address */

    { int _irql = xbox_IrqlEnterInterrupt(16); fn(); xbox_IrqlLeaveInterrupt(_irql); }

    xbox_worker_stack_free(slot);
    return (int)(g_eax & 1u);
}

/* Set the status bits and, if the driver has unmasked them, call the ISR.
 *
 * MIE is the master enable and HcInterruptEnable is the per-source mask; a
 * controller that interrupts through either of those while they are clear is
 * a controller the driver has every right to be confused by.
 */
static void ohci_raise(OhciController *hc, uint32_t source)
{
    uint32_t enable;
    int claimed;
    bus_lock();
    enable = hc->reg[HcInterruptEnable / 4];
    source &= hc->reg[HcInterruptStatus / 4] & enable;
    bus_unlock();
    if (!(enable & INTR_MIE) || !source) return;

    claimed = ohci_call_isr(hc);
    if (s_trace || claimed >= 0) {
        static unsigned n;
        if (n++ < 20) {
            fprintf(stderr, "  [OHCI%d] raised %08X -> ISR %s\n",
                    hc->index, source,
                    claimed < 0 ? "not callable" :
                    claimed ? "claimed it" : "declined it");
            fflush(stderr);
        }
    }
}

/* ---- bring-up ---------------------------------------------------------- */

static void ohci_reset(OhciController *hc, uint32_t base, int index)
{
    memset(hc, 0, sizeof *hc);
    hc->base  = base;
    hc->index = index;

    hc->reg[HcRevision / 4]       = 0x00000010u;   /* OHCI 1.0             */
    hc->reg[HcFmInterval / 4]     = 0x27782EDFu;   /* 11999, FSMPS default */
    hc->reg[HcPeriodicStart / 4]  = 0x00003E67u;   /* 90% of the frame     */
    hc->reg[HcLSThreshold / 4]    = 0x00000628u;

    /* Root hub: OHCI_PORTS downstream, ports always powered, no over-current
     * reporting. NoPowerSwitching keeps a driver from waiting on a power-on
     * sequence that has nothing to switch. */
    hc->reg[HcRhDescriptorA / 4]  = (uint32_t)s_ndp | (1u << 9);
    hc->reg[HcRhDescriptorB / 4]  = 0x00000000u;
    hc->reg[HcRhStatus / 4]       = 0x00000000u;

    /* Ports powered and empty. The gamepad is not here yet, deliberately.
     *
     * Presenting it as already connected does not work, and the reason is
     * worth keeping: the driver scans the root hub itself during bring-up,
     * sees the connect-status-change bit, clears it, and by the time it
     * unmasks the root hub interrupt there is no change left to report. The
     * pending status bit does not survive either, because the driver resets
     * the controller first and a reset clears interrupt status -- both of
     * those are correct behaviour, and between them a device that was always
     * there is a device that never arrives.
     *
     * A console detects the port change after the controller is running, so
     * that is what the controller thread does: it waits for operational with
     * the interrupt unmasked, and only then plugs the device in. */
    /* Every port the descriptor claims, not the first two.
     *
     * A root hub that reports four downstream ports and powers two has two
     * ports a driver will skip: no PortPowerStatus is an unpowered socket,
     * and nothing is expected to arrive on one. That made a device placed on
     * port 3 invisible, which read as "the driver only scans two ports" and
     * is really "we only powered two". */
    {
        unsigned p;
        for (p = 0; p < 4u; p++)
            hc->reg[(HcRhPortStatus1 + p * 4) / 4] =
                (p < s_ndp) ? PORT_PPS : 0u;
    }
}

/* The controller, running on its own thread.
 *
 * It has to be its own thread for a reason that is easy to get wrong: the
 * guest register file is thread-local, so setting g_esp to a worker stack on
 * the guest's thread would overwrite the guest's own stack pointer mid-call.
 * A separate thread has its own copy, and it also happens to be what the
 * hardware does -- an interrupt arrives when the controller decides, not when
 * the driver next reads a register.
 *
 * ponytail: one delivery, of the root hub status change that is already
 * pending from bring-up. There is nothing behind it yet -- no descriptor list
 * walking, so an enumeration attempt has nothing to answer it -- and the point
 * of this delivery is to find out what the driver does when it finally gets
 * the interrupt it has been waiting for. Repeat delivery and the transfer
 * lists come after that answer, not before it.
 */
static DWORD WINAPI ohci_thread(LPVOID unused)
{
    /* This thread calls recompiled code, so it needs what any thread running
     * recompiled code needs: its own TIB. The guest register set is already
     * thread-local and the ISR gets a worker stack, but fs:[0] is the SEH
     * chain head and fs:[4] reaches the CRT's per-thread data -- and a guest
     * function with an SEH prologue on a thread whose g_fs_base is zero
     * dereferences null before it executes a line of its own body. That is
     * what killed the process here, two interrupts in, with no fault report
     * because the fault was in the runtime rather than in the title. */
    int plugged = 0;
    uint32_t last_status = 0, uncleared = 0;
    unsigned repeats = 0;
    int      held_off_warned = 0, held_off_forced = 0;
    unsigned last_ack = 0;
    unsigned first_port = 0;

    (void)unused;
    {
        uint32_t tib = xbox_AllocThreadTib();
        if (!tib) {
            fprintf(stderr, "  [OHCI0] no TIB for the controller thread; "
                            "not delivering interrupts\n");
            fflush(stderr);
            return 0;
        }
        g_fs_base = tib;
    }
    {
        /* It runs the controller's ISR: above DISPATCH_LEVEL (kernel_hal.c). */
        extern void xbox_IrqlInterruptThread(void);
        xbox_IrqlInterruptThread();
    }

    ULONGLONG held_since = 0, start_ms = GetTickCount64(), settle_ms = 0;
    for (;;) {
        OhciController *hc = &s_hc[s_device_hc];
        uint32_t control, enable, status;
        uint32_t pass_done = 0;     /* this pass's done queue, newest first */

        Sleep(1);
        bus_lock();

        /* What the handler left set since the last delivery, read before this
         * pass latches anything new. The stuck-source guard below needs it. */
        uncleared = hc->reg[HcInterruptStatus / 4];

        /* The controller writes the frame number into the HCCA every frame,
         * and a driver reads it from there rather than from the register.
         * Leaving it frozen leaves a driver pacing itself by a clock that
         * never ticks. */
        {
            static uint32_t last_frame = 0xFFFFFFFFu;
            uint32_t hcca = hc->reg[HcHCCA / 4], frame = ohci_frame_now();
            hc->reg[HcFmNumber / 4] = frame;
            if (hcca)
                wr32(hcca + HCCA_FRAME_NO, frame);
            /* Start of frame latches every frame, and a driver that wants to
             * count frames enables it and waits. Def Jam's USB stack does
             * exactly that after SET_ADDRESS, for the address recovery
             * interval; with SF never raised it waited for ever, and the pad
             * was never configured or polled. */
            if (frame != last_frame) {
                last_frame = frame;
                hc->reg[HcInterruptStatus / 4] |= INTR_SF;
            }
        }
        control = hc->reg[HcControl / 4];
        enable  = hc->reg[HcInterruptEnable / 4];

        /* Operational is HCFS == 10b in bits 7:6. Interrupting a controller
         * the driver has not started yet is not a test of anything. */
        if ((control & 0xC0u) != 0x80u) {
            held_since = 0;
            if (!plugged && GetTickCount64() - start_ms > 30000) {
                bus_unlock(); break;
            }
            bus_unlock(); continue;
        }
        s_loop_passes++;
        if (!(enable & INTR_MIE)) {
            held_since = 0;
            /* The handler masks the controller and its deferred routine
             * unmasks it, within milliseconds. Two seconds masked is a
             * deferred routine that never ran, and a pad that is dead. */
            static int told;
            ULONGLONG t = GetTickCount64();
            s_loop_masked++;
            if (!s_masked_since)
                s_masked_since = t;
            else if (t - s_masked_since > 2000 && told < 5) {
                told++;
                s_masked_since = t;
                fprintf(stderr, "  [OHCI0] interrupts masked for 2 s: the USB deferred routine has not"
                                " run; the pad is not being polled (#%d)\n", told);
                fflush(stderr);
            }
            bus_unlock(); continue;
        }
        s_masked_since = 0;

        /* Plug the device in once, after the driver is running and listening.
         * Presenting it earlier does not work: the driver clears the connect
         * change during its own bring-up scan, and a reset clears interrupt
         * status, so a device that was always there is one that never
         * arrives. */
        if (!plugged && (enable & INTR_RHSC)) {
            unsigned port = pad_root_port();
            hc->reg[(HcRhPortStatus1 + port * 4) / 4] |= PORT_CCS | PORT_CSC;
            hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
            plugged = 1;
            s_plugged_pads = 1;
            first_port = port;
            fprintf(stderr, "  [OHCI0] operational after %u ms; device "
                            "arriving on root port %u\n", (unsigned)(GetTickCount64() - start_ms), pad_root_port() + 1);
            fflush(stderr);
        }

        /* More pads, one at a time: each arrives after the previous one is
         * configured, on the next port, so the driver enumerates them in
         * turn and there is only ever one device at the default address.
         * RECOMP_USB_PADS sets how many (1-4). */
        if (plugged && s_plugged_pads < s_npads
            && usb_gamepad_configured(s_plugged_pads - 1)) {
            if (!settle_ms) settle_ms = GetTickCount64();
            if (GetTickCount64() - settle_ms > 1000) {                 /* ~1 s after the last */
                unsigned port = (first_port + (unsigned)s_plugged_pads) % OHCI_PORTS;
                hc->reg[(HcRhPortStatus1 + port * 4) / 4] |= PORT_CCS | PORT_CSC;
                hc->reg[HcInterruptStatus / 4] |= INTR_RHSC;
                fprintf(stderr, "  [OHCI0] pad %d arriving on port %u%c",
                        s_plugged_pads + 1, port + 1, 10);
                fflush(stderr);
                s_plugged_pads++;
                settle_ms = 0;
            }
        }

        /* Be the bus master. ControlListEnable in HcControl says the driver
         * wants the list walked; ControlListFilled says it has put something
         * on it. Walking on the tick rather than only when CLF is written
         * costs a read of a guest dword and means a descriptor queued without
         * rewriting CLF is still moved -- which is legal, and drivers do it. */
        /* Not while the driver still owes us an acknowledgement.
         *
         * HccaDoneHead belongs to the driver from the moment WDH is set
         * until it clears it. Writing a second done list over the first
         * loses every descriptor on it -- the driver walks a chain whose
         * links now point into the new list, and the structures it keeps
         * alongside go with it. At a twenty-millisecond tick that overlap
         * was rare enough to look like bad luck; at four it corrupted the
         * host controller's pending list within a second or two of the
         * gamepad's reports starting. Leaving the descriptors on their
         * endpoints for another pass costs a few milliseconds of latency
         * and is what the controller is specified to do. */
        if (hc->reg[HcInterruptStatus / 4] & INTR_WDH)
            goto deliver;

        /* PeriodicListEnable, bit 2 of HcControl. */
        if (control & 0x04u) {
            if (!hc->ple_seen) {
                hc->ple_seen = 1;
                fprintf(stderr, "  [OHCI%d] periodic list enabled "
                        "(HcControl=%08X HCCA=%08X)\n",
                        hc->index, control, hc->reg[HcHCCA / 4]);
                fflush(stderr);
            }
            ohci_run_periodic(hc, &pass_done);
        }

        /* BulkListEnable, bit 5. Nothing on this device uses bulk, but the
         * list is walked the same way and a driver that puts a transfer
         * there is owed the same service. */
        if ((control & 0x20u)
         && (hc->reg[HcBulkHeadED / 4] & ED_PTR_MASK) && guest_ok(phys_to_guest(hc->reg[HcBulkHeadED / 4] & ED_PTR_MASK), 16)) {
            if (ohci_run_list(hc, hc->reg[HcBulkHeadED / 4], 0, &pass_done))
                hc->reg[HcCommandStatus / 4] &= ~0x04u;   /* BLF consumed */
        }

        if ((control & 0x10u)
         && (hc->reg[HcControlHeadED / 4] & ED_PTR_MASK) && guest_ok(phys_to_guest(hc->reg[HcControlHeadED / 4] & ED_PTR_MASK), 16)) {
            if (ohci_run_list(hc, hc->reg[HcControlHeadED / 4], 0, &pass_done))
                hc->reg[HcCommandStatus / 4] &= ~0x02u;   /* CLF consumed */
        }

        /* One done queue per pass, covering all three lists.
         *
         * The lists used to publish one after another, so the control list
         * wrote HccaDoneHead straight over what the periodic list had just
         * put there. That stayed invisible until a title used both at once:
         * Burnout 3 sends its pad a rumble report (SET_REPORT, 21 09) on the
         * control pipe every frame once its menus are up, the input report
         * completing in the same pass was lost, and the driver -- which
         * re-arms the interrupt endpoint from that completion -- stopped
         * polling the pad. A real controller accumulates everything that
         * retires in a frame and writes the queue back once; so does this. */
        if (pass_done)
            ohci_publish_done(hc, pass_done);

        /* Level-triggered, which is what OHCI is: while an enabled source is
         * set, the line is asserted. The handler clears the status bit, so
         * this stops on its own -- and if it ever does not, the cap below says
         * so rather than spinning the ISR forever. */
    deliver:
        status = hc->reg[HcInterruptStatus / 4] & enable & 0x7Fu;
        if (!status) { held_since = 0; bus_unlock(); continue; }

        /* Bounded, because the mask is a model and not the hardware.
         *
         * The depth is a count of raise/lower pairs across every thread, and
         * one unmatched raise anywhere leaves the gate shut for the rest of
         * the run -- which is how this first showed up: no interrupt was ever
         * delivered and enumeration stopped at two empty control EDs. Real
         * hardware is never masked for 80 ms, so past that the line is
         * asserted anyway. The common case still holds the ISR out of the
         * guest's critical section; the pathological case costs a delay
         * instead of the device. */
        if (xbox_IrqlBlocksInterrupts() && (!held_since || GetTickCount64() - held_since <= OHCI_IRQ_HOLDOFF_MS)) {
            if (!held_since) held_since = GetTickCount64();
            if (!held_off_warned) {
                held_off_warned = 1;
                fprintf(stderr, "  [OHCI%d] irq held off by guest IRQL "
                        "(depth %d, status=0x%02X)\n",
                        hc->index, xbox_IrqlRaisedCount(), status);
                fflush(stderr);
            }
            bus_unlock(); continue;
        }
        if (held_since && GetTickCount64() - held_since > OHCI_IRQ_HOLDOFF_MS && !held_off_forced) {
            held_off_forced = 1;
            fprintf(stderr, "  [OHCI%d] guest IRQL never dropped (depth %d); "
                    "delivering anyway\n", hc->index, xbox_IrqlRaisedCount());
            fflush(stderr);
        }
        held_since = 0;

        /* A stuck source is one the handler never clears: the same bits still
         * set when the next pass comes round. Counting deliveries alone trips
         * on a device that is simply busy -- a pad reporting and a title
         * sending rumble complete a transfer, and so raise writeback-done,
         * many times a second, and the handler clears each one. That tripped
         * this guard a minute or two into every run and stopped input dead.
         * Start of frame re-latches every frame by design, so it is no sign
         * of a handler that never clears either. */
        if ((status & ~INTR_SF) && (status & ~INTR_SF) == (last_status & ~INTR_SF)
                && ((last_status & ~INTR_SF) & ~uncleared) == 0) {
            if (++repeats > 200) {
                fprintf(stderr, "  [OHCI%d] status %08X delivered 200 times "
                                "with no acknowledgement; stopping\n",
                        hc->index, status);
                fflush(stderr);
                bus_unlock(); break;
            }
        } else {
            last_status = status;
            last_ack    = hc->ack_seq;
            repeats     = 0;
        }
        bus_unlock();
        ohci_raise(hc, status);
    }
    return 0;
}

void xbox_UsbSetPadCount(int n)
{
    if (n > USB_GAMEPAD_MAX) n = USB_GAMEPAD_MAX;
    if (n > s_npads) s_npads = n;
}

void xbox_OhciInit(void)
{
    static int done;

    if (done)
        return;
    done = 1;

    /* Opt-in. Without a descriptor list walker behind it a driver that finds
     * a port has nothing to enumerate, so this must not change how a title
     * behaves until the rest of it exists. */
    if (!getenv("RECOMP_USB"))
        return;

    s_enabled = 1;
    s_trace   = getenv("RECOMP_USB_TRACE") != NULL;
    {
        const char *hcspec = getenv("RECOMP_USB_HC");
        const char *ndpspec = getenv("RECOMP_USB_NDP");
        s_device_hc = (hcspec && atoi(hcspec) == 1) ? 1 : 0;
        const char *padspec = getenv("RECOMP_USB_PADS");
        if (ndpspec) {
            int n = atoi(ndpspec);
            if (n >= 1 && n <= 4) s_ndp = (unsigned)n;
        }
        if (padspec) {
            int n = atoi(padspec);
            if (n >= 1 && n <= USB_GAMEPAD_MAX) s_npads = n;
        }
        if ((unsigned)s_npads > s_ndp)
            s_ndp = (unsigned)s_npads;      /* a port for every pad */
    }
    {
        int pad;
        for (pad = 0; pad < s_npads; pad++) {
            unsigned root = (pad_root_port() + (unsigned)pad) % OHCI_PORTS;
            if (s_ndp <= root) s_ndp = root + 1;
        }
    }
    ohci_reset(&s_hc[0], XBOX_OHCI0_BASE, 0);
    ohci_reset(&s_hc[1], XBOX_OHCI1_BASE, 1);

    /* The registers have to fault to be answered. The MCPX aperture is mapped
     * as plain committed memory, so both blocks are made inaccessible here and
     * the title's VEH routes the faults back to xbox_OhciHandleMmio.
     *
     * A failed protect switches the model off rather than leaving it half on:
     * a controller whose registers read as zero out of RAM is exactly the
     * situation this exists to end, and it would look identical. */
    {
        ptrdiff_t off = xbox_GetMemoryOffset();
        int i;

        if (!off) {
            s_enabled = 0;
            fprintf(stderr, "  OHCI: guest memory not mapped yet; disabled\n");
            return;
        }
        for (i = 0; i < 2; i++) {
            DWORD old_protect;
            LPVOID at = (LPVOID)((uintptr_t)off + s_hc[i].base);
            if (!VirtualProtect(at, XBOX_OHCI_SIZE, PAGE_NOACCESS,
                                &old_protect)) {
                s_enabled = 0;
                fprintf(stderr, "  OHCI: cannot trap 0x%08X (error %lu); "
                                "disabled\n", s_hc[i].base, GetLastError());
                return;
            }
        }
    }

    fprintf(stderr, "  OHCI: two controllers at 0x%08X and 0x%08X, "
                    "%d ports each, one device on HC0 root port %u\n",
            XBOX_OHCI0_BASE, XBOX_OHCI1_BASE, OHCI_PORTS, pad_root_port() + 1);
    fflush(stderr);

    {
        HANDLE th = CreateThread(NULL, 0, ohci_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    }
}

static OhciController *hc_for(uint32_t va)
{
    int i;

    if (!s_enabled)
        return NULL;
    for (i = 0; i < 2; i++)
        if (va >= s_hc[i].base && va < s_hc[i].base + XBOX_OHCI_SIZE)
            return &s_hc[i];
    return NULL;
}

int xbox_OhciOwnsAddress(uint32_t xbox_va)
{
    return hc_for(xbox_va) != NULL;
}

int xbox_OhciHandleMmio(void *ctx, uint32_t xbox_va)
{
    OhciController *hc = hc_for(xbox_va);
    int ok;

    if (!hc)
        return 0;
    /* The list-head registers get written twice during bring-up, the second
     * time with a value that is not a descriptor. Naming the instruction
     * that does it is the only way to tell a driver quirk from a defect in
     * how this runtime answers it. */
#if defined(_WIN32)
    s_last_write_rip = (uint64_t)((PCONTEXT)ctx)->Rip;
#else
    s_last_write_rip = (uint64_t)mmio_trap_pc(ctx);
#endif
    ok = mmio_emulate(ctx, xbox_va - hc->base, hc,
                      ohci_read, ohci_write);
    if (!ok && hc->decode_fail++ < 20) {
        const uint8_t *ip = (const uint8_t *)(uintptr_t)s_last_write_rip;
        fprintf(stderr, "  [OHCI%d] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                hc->index, xbox_va - hc->base,
                ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
}

void xbox_OhciReport(void)
{
    int i;

    if (!s_enabled)
        return;
    for (i = 0; i < 2; i++)
        fprintf(stderr, "  [OHCI%d] %u reads, %u writes, %u undecoded; "
                        "HcControl=%08X HcIntStatus=%08X pad port=%08X\n",
                i, s_hc[i].reads, s_hc[i].writes, s_hc[i].decode_fail,
                s_hc[i].reg[HcControl / 4],
                s_hc[i].reg[HcInterruptStatus / 4],
                s_hc[i].reg[(HcRhPortStatus1 + 4 * pad_root_port()) / 4]);
    fflush(stderr);
}
