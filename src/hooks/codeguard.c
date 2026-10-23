/*
 * Notice when the title writes over its own code.
 *
 * Nothing should ever write to an executable section. When something does,
 * what breaks is whatever happens to live there, and it breaks much later and
 * somewhere else: a switch whose jump table has been overwritten tail-jumps to
 * a byte of a string and the title falls over three functions away, with
 * nothing at the scene to say why. That has happened twice here -- once over
 * the jump table at guest 0x00077158, once over the main thread block -- and
 * both times the cause was found only because the address was known in advance
 * and could be watched. The debug registers watch four addresses; this watches
 * every executable byte in the image.
 *
 * It keeps a copy of each executable section and compares on a timer, which is
 * a sampling detector: it reports that a word changed and when it was noticed,
 * not the instruction that changed it. That is still most of the answer,
 * because the address is what a RECOMP_WATCH_WRITE needs to name the writer,
 * and finding the address is the part that costs a day.
 *
 *     $env:RECOMP_GUARD_CODE="1"        guard every executable section
 *     $env:RECOMP_GUARD_MS="250"        how often to compare (default 500)
 *
 * Why this matters in a static recompilation, where it might look as though
 * it should not: overwriting guest code is harmless here, because the code
 * that runs is compiled C and no longer reads those bytes. What is not
 * harmless is the DATA that lives in an executable section -- above all the
 * jump tables, which the lifted code does read at run time, once per switch.
 * A corrupted jump table is a switch that jumps to a byte of a string. So
 * the reports worth acting on are the ones that land on data, and reports
 * that land on instruction bytes can be noted and set aside.
 *
 * For the same reason there are false positives that are not bugs at all: an
 * XBE section marked executable holds both code and writable data, and a
 * title writing its own statics there is behaving normally. Expect noise,
 * and read the addresses rather than the count.
 *
 * The sections come from the image's own headers, read out of guest memory
 * rather than assumed, so a title whose code does not start where this one's
 * does is handled without a change here.
 */

#include "host.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define GUARD_MAX_SECTIONS 16
#define GUARD_MAX_REPORTS  4000

/* XBE section header flag: the section is executable. */
#define XBE_SECTION_EXECUTABLE 0x00000004u

typedef struct {
    uint32_t va, size;
    uint8_t *copy;
} GuardSection;

static GuardSection s_section[GUARD_MAX_SECTIONS];
static int      s_sections;
static DWORD    s_interval = 500;
static int      s_reports;
static DWORD    s_delay = 8000;

static const void *guest_ptr(uint32_t va)
{
    return (const void *)((uintptr_t)va + (uintptr_t)g_xbox_mem_offset);
}

static uint32_t guest_u32(uint32_t va)
{
    return *(const volatile uint32_t *)guest_ptr(va);
}

/* Take the executable sections from the loaded image's own headers. */
static void guard_collect(uint32_t base)
{
    uint32_t count, headers, i;

    if (memcmp(guest_ptr(base), "XBEH", 4) != 0) {
        fprintf(stderr, "  [GUARD] no image header at 0x%08X; not guarding\n",
                base);
        return;
    }
    count   = guest_u32(base + 0x11C);
    headers = guest_u32(base + 0x120);
    if (!count || count > 64 || !headers)
        return;

    for (i = 0; i < count && s_sections < GUARD_MAX_SECTIONS; i++) {
        uint32_t h     = headers + i * 0x38;
        uint32_t flags = guest_u32(h + 0x00);
        uint32_t va    = guest_u32(h + 0x04);
        uint32_t size  = guest_u32(h + 0x08);

        if (!(flags & XBE_SECTION_EXECUTABLE) || !va || !size)
            continue;
        /* A section can be huge; the copy is worth it for the one thing it
         * catches, but cap it so a malformed header cannot ask for a
         * gigabyte. */
        if (size > 8u * 1024u * 1024u)
            size = 8u * 1024u * 1024u;

        s_section[s_sections].copy = (uint8_t *)malloc(size);
        if (!s_section[s_sections].copy)
            return;
        memcpy(s_section[s_sections].copy, guest_ptr(va), size);
        s_section[s_sections].va   = va;
        s_section[s_sections].size = size;
        s_sections++;
        fprintf(stderr, "  [GUARD] watching 0x%08X..0x%08X (%u KB)\n",
                va, va + size, size / 1024u);
    }
}

static DWORD WINAPI guard_thread(LPVOID unused)
{
    (void)unused;

    /* Snapshot once the title is up, not at startup.
     *
     * The loader patches the image's kernel import table in place -- that is
     * executable memory changing, legitimately, and there is enough of it to
     * fill any report budget before anything interesting happens. Waiting
     * until the title is running means the copy is taken of a settled image
     * and every later difference is worth a line. */
    Sleep(s_delay);
    guard_collect(0x00010000u);
    if (!s_sections) {
        fprintf(stderr, "  [GUARD] no executable sections found\n");
        fflush(stderr);
        return 0;
    }

    for (;;) {
        int i;

        Sleep(s_interval);
        for (i = 0; i < s_sections && s_reports < GUARD_MAX_REPORTS; i++) {
            const uint32_t *now = (const uint32_t *)guest_ptr(s_section[i].va);
            uint32_t *was = (uint32_t *)s_section[i].copy;
            uint32_t words = s_section[i].size / 4, w;

            for (w = 0; w < words && s_reports < GUARD_MAX_REPORTS; w++) {
                if (now[w] == was[w])
                    continue;
                fprintf(stderr, "  [GUARD] executable memory changed at guest "
                                "0x%08X: 0x%08X -> 0x%08X\n",
                        s_section[i].va + w * 4, was[w], now[w]);
                was[w] = now[w];      /* report each change once */
                s_reports++;
            }
        }
        fflush(stderr);
        if (s_reports >= GUARD_MAX_REPORTS) {
            fprintf(stderr, "  [GUARD] %d reports; stopping\n", s_reports);
            fflush(stderr);
            return 0;
        }
    }
}

int codeguard_init(void)
{
    const char *want = getenv("RECOMP_GUARD_CODE");
    const char *ms   = getenv("RECOMP_GUARD_MS");

    if (!want || !*want)
        return 0;
    {
        const char *d = getenv("RECOMP_GUARD_DELAY_MS");
        if (d && *d)
            s_delay = (DWORD)strtoul(d, NULL, 0);
    }
    if (ms && *ms) {
        DWORD v = (DWORD)strtoul(ms, NULL, 0);
        if (v >= 50)
            s_interval = v;
    }

    /* The image base is where the loader put it; 0x00010000 is what every
     * title this runtime has seen uses, and the header check below rejects
     * the guess rather than guarding rubbish. */
    if (!CreateThread(NULL, 0, guard_thread, NULL, 0, NULL)) {
        fprintf(stderr, "  [GUARD] could not start (error %lu)\n",
                GetLastError());
        return 0;
    }
    return 1;
}
