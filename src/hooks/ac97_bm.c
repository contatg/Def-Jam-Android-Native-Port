/*
 * AC'97 bus-master registers, enough of them to stop waiting.
 *
 * This title's DirectSound is statically linked into the XBE, so it was
 * recompiled along with the game and still drives the audio controller's
 * registers itself. One of those handshakes cannot be satisfied by memory:
 *
 *     0x00267B5A   mov byte ptr [reg], 2        ; CR.RR = 1, reset this channel
 *     0x00267B6A   mov cl, byte ptr [reg]       ; read it back
 *     0x00267B70   and cl, 2
 *     0x00267B73   test cl, cl
 *     0x00267B75   jne  0x00267B73              ; spin while the bit is set
 *
 * RR is "reset registers", which real hardware clears once the channel reset
 * completes. Against plain memory it stays set, the title latches it into cl
 * six instructions after writing it, and then loops on the register forever.
 * Nothing written to memory afterwards can reach a value already in a
 * register, so acknowledging the bit on a timer does not release this: the
 * read has to come back clear, which means the write must never set it.
 *
 * So the page is trapped and the writes are serviced here. Reads and writes
 * are decoded by the runtime's own mmio_decode.h, the same helper its USB
 * controller model uses, so this is only the register behaviour and none of
 * the instruction decoding.
 *
 * Scope is deliberately small. Every register keeps whatever the title puts
 * in it, and the single exception is RR, which reads back clear because no
 * reset is pending here and never will be. That is the honest answer rather
 * than a convenient one: this runtime has no DMA engine to reset.
 */

#include "../host.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mmio_decode.h"

/* The controller's registers sit at 0xFEC00000. The bus-master block starts at
 * +0x100 and repeats every 0x10 bytes per DMA channel; a second block lives at
 * +0x2100. Only those two pages are trapped, so the codec registers further up
 * (and the ready bit the runtime sets in them) keep working as plain memory. */
#define AC97_BASE        0xFEC00000u
#define AC97_PAGE        0x1000u
#define AC97_BM_CR       0x0Bu     /* control register, per channel */
#define AC97_CR_RR       0x02u     /* reset registers; hardware self-clears it */

typedef struct {
    uint32_t base;                 /* guest VA of this page */
    uint8_t  shadow[AC97_PAGE];    /* register state we serve */
    uint32_t reads, writes, undecoded, rr_masked;
} Ac97Page;

static Ac97Page s_pages[2];
static volatile LONG s_page_count;

static Ac97Page *page_for(uint32_t va)
{
    int i;
    LONG count = InterlockedCompareExchange(&s_page_count, 0, 0);
    for (i = 0; i < count; i++)
        if (va >= s_pages[i].base && va < s_pages[i].base + AC97_PAGE)
            return &s_pages[i];
    return NULL;
}

/* A bus-master control register: +0x0B within each 0x10-byte channel block,
 * inside the 0x100-0x1FF window of its page. */
static int is_channel_control(uint32_t off)
{
    return off >= 0x100u && off < 0x200u && (off & 0x0Fu) == AC97_BM_CR;
}

static uint64_t ac97_read(void *dev, uint32_t off, int size)
{
    Ac97Page *p = (Ac97Page *)dev;
    uint64_t v = 0;
    int i;

    p->reads++;
    for (i = 0; i < size && off + i < AC97_PAGE; i++)
        v |= (uint64_t)p->shadow[off + i] << (8 * i);
    return v;
}

static void ac97_write(void *dev, uint32_t off, uint64_t val, int size)
{
    Ac97Page *p = (Ac97Page *)dev;
    int i;

    p->writes++;
    if (p->writes <= 16) {
        fprintf(stderr, "  [AC97] write +0x%03X = %0*llX\n",
                off, size * 2, (unsigned long long)val);
        fflush(stderr);
    }
    for (i = 0; i < size && off + i < AC97_PAGE; i++) {
        uint8_t b = (uint8_t)(val >> (8 * i));
        if (is_channel_control(off + i) && (b & AC97_CR_RR)) {
            /* Take the reset, report it finished. Bit 0 is run/pause, which is
             * the title's own state and is left exactly as written. */
            b = (uint8_t)(b & ~AC97_CR_RR);
            p->rr_masked++;
        }
        p->shadow[off + i] = b;
    }
}

int ac97_bm_owns_address(uint32_t xbox_va)
{
    return page_for(xbox_va) != NULL;
}

int ac97_bm_handle_mmio(void *ctx, uint32_t xbox_va)
{
    Ac97Page *p = page_for(xbox_va);
    int ok;

    if (!p)
        return 0;
    ok = mmio_emulate((PCONTEXT)ctx, xbox_va - p->base, p, ac97_read, ac97_write);
    if (!ok && p->undecoded++ < 10) {
        const uint8_t *ip = (const uint8_t *)(uintptr_t)HOST_PC(ctx);
        fprintf(stderr, "  [AC97] undecoded access at +0x%03X: "
                        "%02X %02X %02X %02X %02X %02X\n",
                xbox_va - p->base, ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
        fflush(stderr);
    }
    return ok;
}

/* Trap the two pages, seeding each shadow from whatever is already there so
 * anything the runtime set up beforehand survives. Call after the memory
 * layout exists; harmless to call twice. */
void ac97_bm_init(ptrdiff_t mem_offset)
{
    static const uint32_t bases[2] = { AC97_BASE, AC97_BASE + 0x2000u };
    size_t i;

    if (s_page_count)
        return;

    for (i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        void *native = (void *)((uintptr_t)bases[i] + (uintptr_t)mem_offset);
        DWORD old = 0;

        memcpy(s_pages[s_page_count].shadow, HOST_VIEW(native), AC97_PAGE);
        s_pages[s_page_count].base = bases[i];
        /* Polling threads are already running. Publish initialized ownership
         * before protection can fault, as the NV2A page setup does. */
        InterlockedIncrement(&s_page_count);
        if (!VirtualProtect(native, AC97_PAGE, PAGE_NOACCESS, &old)) {
            fprintf(stderr, "  [AC97] could not trap 0x%08X (error %lu); "
                            "the reset handshake will spin\n",
                    bases[i], GetLastError());
            InterlockedDecrement(&s_page_count);
            continue;
        }
    }

    if (s_page_count)
        fprintf(stderr, "  AC97: %ld bus-master page(s) trapped from 0x%08X "
                        "(channel resets report complete)\n",
                s_page_count, AC97_BASE);
    fflush(stderr);
}

void ac97_bm_report(void)
{
    int i;
    for (i = 0; i < s_page_count; i++)
        fprintf(stderr, "  [AC97] page 0x%08X: %u reads, %u writes, "
                        "%u resets answered, %u undecoded\n",
                s_pages[i].base, s_pages[i].reads, s_pages[i].writes,
                s_pages[i].rr_masked, s_pages[i].undecoded);
    fflush(stderr);
}
