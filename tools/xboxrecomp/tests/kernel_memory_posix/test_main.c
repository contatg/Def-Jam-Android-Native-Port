/*
 * The kernel's memory APIs on a POSIX host -- written to fail.
 *
 * These sit on the Win32 shim rather than on the Xbox layout, and the shim's
 * VirtualQuery is a constant: it reports RegionSize 0x1000, MEM_COMMIT,
 * PAGE_READWRITE and AllocationBase NULL for every address it is ever handed,
 * mapped or not. Four kernel entry points are built on that answer, and one of
 * them picks a deallocator with it.
 *
 *   1. free/alloc pairing  MmAllocateContiguousMemory always uses VirtualAlloc
 *                          (mmap here). MmFreeContiguousMemory routes to
 *                          VirtualFree only when AllocationBase == the
 *                          pointer, which a hardcoded NULL never satisfies --
 *                          so every contiguous buffer is released with free()
 *                          on an mmap'd pointer
 *   2. AllocationBase      must name the allocation a pointer belongs to
 *   3. unmapped addresses  must not be reported as committed and readable
 *   4. MmQueryAllocationSize  must not answer 0x1000 for every allocation
 *
 * Checks that can abort the process run in a forked child, because the failure
 * mode for (1) is the allocator calling abort(), which would take the whole
 * run with it and report nothing about the other three.
 */
#include "kernel.h"
#include "win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

/* The generated title provides this; a regression harness has no title. The
 * kernel bridge references it, and a static archive only resolves what is
 * pulled in, so linking any translation unit that reaches the bridge needs a
 * stand-in. Nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-58s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* Run fn in a child; return 1 if it exited cleanly, 0 if it died or failed.
 * An allocator abort is a SIGABRT, not a return value, so it cannot be
 * observed in-process. */
static int survives_in_child(void (*fn)(void), char *how, size_t howlen)
{
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        fn();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        /* Name the signal: "it crashed" and "the allocator rejected the
         * pointer" are different findings, and only one of them is this bug. */
        snprintf(how, howlen, "child died from signal %d (%s)",
                 WTERMSIG(status),
                 WTERMSIG(status) == SIGABRT ? "SIGABRT -- the allocator "
                                               "rejected the pointer"
                                             : strsignal(WTERMSIG(status)));
        return 0;
    }
    if (WEXITSTATUS(status) != 0) {
        snprintf(how, howlen, "child exited %d", WEXITSTATUS(status));
        return 0;
    }
    return 1;
}

static void alloc_then_free(void)
{
    /* The pairing a title makes: ask the kernel for a GPU-visible buffer,
     * use it, hand it back. */
    PVOID p = xbox_MmAllocateContiguousMemory(64 * 1024);
    if (!p) _exit(2);
    memset(p, 0xA5, 64 * 1024);
    xbox_MmFreeContiguousMemory(p);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    /* 1. The alloc/free pair must not kill the process. */
    {
        char how[160] = "";
        check(survives_in_child(alloc_then_free, how, sizeof how),
              "MmAllocateContiguousMemory then MmFreeContiguousMemory survives",
              how);
    }

    /* 2. VirtualQuery must identify a region the shim itself allocated. */
    {
        SIZE_T want = 64 * 1024;
        LPVOID p = VirtualAlloc(NULL, want, MEM_COMMIT | MEM_RESERVE,
                                PAGE_READWRITE);
        check(p != NULL, "VirtualAlloc returns a pointer", NULL);
        if (p) {
            MEMORY_BASIC_INFORMATION mbi;
            memset(&mbi, 0, sizeof mbi);
            SIZE_T got = VirtualQuery(p, &mbi, sizeof mbi);

            check(got == sizeof mbi, "VirtualQuery reports on a live mapping",
                  "query failed outright");
            check(mbi.AllocationBase == p,
                  "VirtualQuery names the allocation base",
                  "AllocationBase is not the pointer, so callers that key off "
                  "it pick the wrong deallocator");
            check(mbi.RegionSize >= want,
                  "VirtualQuery reports the real region size",
                  "RegionSize is the hardcoded 0x1000");
            check(mbi.State == MEM_COMMIT,
                  "VirtualQuery reports committed memory as committed", NULL);

            /* An address *inside* the region must resolve to it. Callers
             * query interior addresses at least as often as bases -- a
             * translated guest VA lands in the middle of the 64 MB window,
             * never at its start. */
            {
                MEMORY_BASIC_INFORMATION mid;
                memset(&mid, 0, sizeof mid);
                unsigned char *interior = (unsigned char *)p + 4096;
                SIZE_T n = VirtualQuery(interior, &mid, sizeof mid);
                check(n == sizeof mid && mid.AllocationBase == p,
                      "VirtualQuery resolves an interior address to its region",
                      "only exact base addresses are recognised");
                check(mid.RegionSize == want - 4096,
                      "interior region size runs to the end of the region",
                      "RegionSize is not measured from the queried address");
            }

            /* 4. And the kernel wrapper built on it must agree. */
            ULONG sz = xbox_MmQueryAllocationSize(p);
            check((SIZE_T)sz >= want,
                  "MmQueryAllocationSize returns the allocation's size",
                  "answers 0x1000 for every pointer");

            VirtualFree(p, 0, MEM_RELEASE);
        }
    }

    /* 3. An address nothing ever mapped must not read as committed. A high
     * canonical address well clear of anything the process maps. */
    {
        MEMORY_BASIC_INFORMATION mbi;
        memset(&mbi, 0, sizeof mbi);
        void *nowhere = (void *)(uintptr_t)0x0000700000000000ull;
        SIZE_T got = VirtualQuery(nowhere, &mbi, sizeof mbi);
        check(!(got == sizeof mbi && mbi.State == MEM_COMMIT),
              "VirtualQuery does not report unmapped memory as committed",
              "every address answers MEM_COMMIT/PAGE_READWRITE, so a guest "
              "probing before it writes is always told yes");
    }

    /*
     * 6. MmGetPhysicalAddress must translate.
     *
     * Ordinal 173 used to have two implementations that disagreed.
     * bridge_MmGetPhysicalAddress subtracted XBOX_CONTIG_BASE for addresses
     * inside the contiguous window, because a DMA consumer needs the physical
     * offset rather than the virtual-window address; xbox_MmGetPhysicalAddress
     * returned its argument unchanged, commented as a placeholder. The bridge
     * now calls this function, so there is one implementation and the expected
     * values below are it.
     *
     * The failure was silent. A title writes the result into a pushbuffer or
     * DMA descriptor, the NV2A reads an address with bit 31 set, and the
     * corruption surfaces as wrong geometry with nothing pointing back here.
     *
     * What this does NOT cover: the bridge itself. bridge_* functions are
     * static and driven by guest CPU state, and nothing in tests/ can reach
     * them -- breaking bridge_MmGetPhysicalAddress alone leaves every check
     * here green. The bridge calling this function is what ties the two
     * together; it is not something these checks verify.
     */
    {
        const struct { uint32_t in, want; const char *what; } cases[] = {
            { 0x80000000u, 0x00000000u, "contiguous base maps to physical 0" },
            { 0x80001000u, 0x00001000u, "inside the contiguous window" },
            { 0x83FFFFFFu, 0x03FFFFFFu, "last byte of the window" },
            { 0x84000000u, 0x84000000u, "just past the window is unchanged" },
            { 0x00040000u, 0x00040000u, "an ordinary guest VA is unchanged" },
        };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            ULONG_PTR got =
                xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)cases[i].in);
            char what[96], detail[128];
            snprintf(what, sizeof what, "MmGetPhysicalAddress: %s",
                     cases[i].what);
            snprintf(detail, sizeof detail,
                     "0x%08X gave 0x%08llX, the bridge gives 0x%08X",
                     cases[i].in, (unsigned long long)got, cases[i].want);
            check((uint32_t)got == cases[i].want, what, detail);
        }
    }

    /* 7. A reserved arena lends its pages to fixed-address requests.
     *
     * Linux places mappings top-down, so the space above an OS-chosen guest
     * base is someone else's, and the contiguous window at base + 2 GB failed
     * on every Steam Deck boot. The layout now reserves the whole guest
     * window and the fixed mappings take it over page by page; that must not
     * turn into a way to map over a live view. */
    {
        const SIZE_T span = 16u << 20, page = 64u << 10;
        uint8_t *base = VirtualAlloc(NULL, span, MEM_RESERVE, PAGE_NOACCESS);
        check(base != NULL, "arena: the span reserves", NULL);
        if (base) {
            uint8_t *a, *b;
            HANDLE m;
            check(win32_reserve_arena(base, span), "arena: registers", NULL);

            /* Without the arena this fails: the span is already mapped. */
            a = VirtualAlloc(base + page, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            check(a == base + page, "arena: a fixed request takes unclaimed pages",
                  "refused, so the layout's fixed mappings would fail as before");
            if (a) a[0] = 0x5A;
            check(VirtualAlloc(base + page, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == NULL,
                  "arena: a claimed page is not handed out twice",
                  "mapped over a live allocation");
            check(VirtualAlloc(base, 2 * page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == NULL,
                  "arena: a request overlapping a claimed page fails", NULL);
            check(a && a[0] == 0x5A, "arena: the live page kept its contents", NULL);

            m = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)page, NULL);
            b = m ? MapViewOfFileEx(m, FILE_MAP_ALL_ACCESS, 0, 0, page, base + 3 * page) : NULL;
            check(b == base + 3 * page, "arena: a view maps at a fixed address inside it", NULL);
            check(MapViewOfFileEx(m, FILE_MAP_ALL_ACCESS, 0, 0, page, base + page) == NULL,
                  "arena: a view cannot replace a live page", NULL);

            /* Released pages go back to the arena, not to the process. */
            check(VirtualFree(a, page, MEM_RELEASE), "arena: a release succeeds", NULL);
            check(VirtualAlloc(base + page, page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) == base + page,
                  "arena: a released page can be claimed again", NULL);
            if (b) UnmapViewOfFile(b);
            if (m) CloseHandle(m);
            check(VirtualFree(base, span, MEM_RELEASE), "arena: the whole span releases", NULL);
        }
    }

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
