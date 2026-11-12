/*
 * A hardware write watchpoint on a guest address.
 *
 * The question this exists for: the title's front-end loop at guest 0x0019AB70
 * idles because its command pointer at 0x002EB73C is null, and nothing in the
 * lifted image ever writes that pointer to a non-null value through a literal
 * address. Either the code that would post a command never runs, or it writes
 * through a pointer. A search of the disassembly cannot tell those apart. A
 * watchpoint can: it names the instruction that writes, whatever addressing it
 * used, and there is no other way to get that answer here.
 *
 * Done with the CPU's debug registers rather than by page-protecting the page,
 * which is the difference between watching four bytes and watching four
 * kilobytes of a title's .data. The lifted code writes that page constantly,
 * so a page trap would report thousands of unrelated writes and run at a
 * crawl.
 *
 * Two halves:
 *
 *   - Debug registers are per thread, and this title creates its threads as it
 *     goes, so a watcher thread re-arms every thread in the process on a timer.
 *     Arming a thread that is already armed is a no-op, so the loop is cheap
 *     and a thread created a minute in is still caught.
 *
 *   - The trap arrives as EXCEPTION_SINGLE_STEP, which the host's own vectored
 *     handler routes here (see veh_handler in src/main.c). This reports the
 *     faulting RIP and the value now at the address, and continues. The RIP is
 *     a host address in an image linked /DYNAMICBASE:NO, so it maps straight
 *     back to a lifted function through build/<preset>/defjam_recomp.map, the
 *     same way scripts/sample-threads.py resolves a sample.
 *
 * Off unless RECOMP_WATCH_WRITE is set to a guest address:
 *
 *     $env:RECOMP_WATCH_WRITE="0x002EB73C"
 *
 * RECOMP_WATCH_RW watches reads as well, which answers "does anything ever
 * look at this" rather than "who changes it".
 *
 * RECOMP_WATCH_EXEC takes a guest FUNCTION address instead and reports every
 * entry to it:
 *
 *     $env:RECOMP_WATCH_EXEC="0x00069F10"
 *
 * This exists because "was this function ever called at all" has been an
 * expensive question here. recomp_lookup_manual() only sees indirect calls, so
 * a wrapper in recomp_manual.c cannot observe a function that its callers
 * reach directly, and naming a function in the lifted C rewrites those call
 * sites rather than intercepting them. An instruction breakpoint does not care
 * how the call was made: the address is resolved through recomp_lookup(), the
 * same guest-to-host mapping the runtime itself dispatches through, and the
 * CPU reports the entry however it was reached.
 *
 * The trap for an instruction breakpoint arrives BEFORE the instruction runs,
 * so the handler sets the resume flag on the way out; without it the same
 * instruction re-traps forever instead of executing.
 *
 * READ THIS BEFORE BELIEVING A NEGATIVE RESULT. A breakpoint fires on entry,
 * and debug registers are per thread and armed by a polling loop, so a
 * function is invisible to it when it was entered before its thread was armed
 * AND has not returned since. That is not a corner case here: the title's
 * long-running loops are entered once during start-up and never return, which
 * is exactly the shape that gets missed. This cost a wrong conclusion already
 * -- the front-end loop at 0x0019AB60 was reported as never entered when it
 * was in fact running the whole time, sitting in its own idle loop, plainly
 * visible on a live stack.
 *
 * So: a hit proves the function ran. A miss proves nothing on its own. Confirm
 * a miss with scripts/guest-stack.py, which reads the stacks as they are
 * rather than watching for an edge, and can see a frame that has been parked
 * there since start-up.
 *
 * THE SAME ASYMMETRY APPLIES TO THE DATA WATCHPOINTS, and not only in theory.
 * A read-write watchpoint on guest 0x803CC934 recorded zero hits across a full
 * run, on an address this runtime's own OHCI model provably writes eight bytes
 * to, six times over -- proved by reading the bytes back out of guest memory
 * in the same run. Why it was missed is not understood. Every data watchpoint
 * that has caught anything here was watching a write made by lifted guest
 * code; the one that missed was watching a write made by host C in the
 * runtime. That is a correlation, not an explanation, and it is recorded here
 * so nobody spends a session reasoning from a silent watchpoint the way one
 * was spent already. Treat a zero-hit result as "not observed", never as
 * "never happened", and corroborate it another way.
 *
 * The arming loop below runs tight for the first few seconds to shrink the
 * window, which helps and does not close it.
 * RECOMP_WATCH_LEN picks 1, 2, 4 or 8 bytes (default 4). The address must be
 * aligned to that length, which is the hardware's rule, not ours.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <tlhelp32.h>

extern ptrdiff_t g_xbox_mem_offset;

/* The runtime's own guest-address-to-callable-host-code mapping. */
typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);

/* The guest's registers, which are thread-local. An entry breakpoint fires on
 * the guest thread itself, so these hold that thread's values at the moment of
 * the call -- which is the only place ecx can be read, and ecx is the object
 * pointer for most of this title's methods. Knowing which object a method was
 * called on is usually the whole question: the code says it reads a field at
 * +0x9C, and without the base that is not an address anyone can look at. */
#if defined(_MSC_VER)
#  define WATCH_TLS __declspec(thread)
#else
#  define WATCH_TLS __thread
#endif
extern WATCH_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern WATCH_TLS uint32_t g_esi, g_edi, g_ebx;


/* RECOMP_WATCH_EXEC_CHAIN follows a pointer chain from ecx at the moment of
 * the call, and prints every step.
 *
 * Reading the same structure with RECOMP_PEEK instead samples it at whatever
 * moment the frame loop gets round to it, and a queue that is filled and
 * drained several times a second is empty at almost every such moment. That
 * produced a snapshot here showing a state field holding a pointer, which is
 * not what the code that reads it expects, and the honest conclusion was that
 * the snapshot had caught the wrong instant rather than that the field was
 * wrong. At the breakpoint there is no such doubt: this is the state the
 * function is about to act on.
 *
 *     $env:RECOMP_WATCH_EXEC_CHAIN="0,0,8"
 *
 * Each step dereferences the current pointer and adds the offset, so "0,0,8"
 * reads *(ecx), then *(*(ecx)), then that plus 8. A null or out-of-range step
 * stops the walk and says so. */
#define WATCH_CHAIN_MAX 8
static int      s_chain[WATCH_CHAIN_MAX];
static int      s_chain_len;

/* RECOMP_WATCH_EXEC_STR prints the text at ecx and at the first argument.
 *
 * Half the interesting functions in this title take a name: a screen to load,
 * a file to open, a variable to set. The pointer alone says nothing, and
 * turning it into text afterwards costs a second run with RECOMP_PEEK at an
 * address that may not hold the same thing by then. Printed here it is the
 * string the call was actually made with. */
static int      s_exec_str;
static int      s_exec_field = -1;
/* RECOMP_WATCH_EXEC_STACK=N prints N stack words at the call, raw.
 *
 * A function reached only through a pointer has no callers for
 * scripts/callers.py to find, and when it runs in a burst that is over by
 * the time anyone samples the process, a live stack read sees nothing. The
 * words are printed unfiltered, as "GS +offset value"; scripts/guest-stack.py
 * --log keeps the ones that are real return addresses. */
static int      s_exec_stack;
static int      s_exec_stack_entries = 8;   /* RECOMP_WATCH_EXEC_STACK_ENTRIES */

static uintptr_t s_address;          /* host address being watched */
static uint32_t  s_guest_va;
/* Execution mode uses all four debug address registers, because the question
 * it answers -- which call in a sequence was the last one reached -- otherwise
 * costs one run of the title per candidate. Four at a time turns a morning of
 * bisecting into a couple of runs. */
#define WATCH_MAX_EXEC 4
static uintptr_t s_exec_addr[WATCH_MAX_EXEC];
static uint32_t  s_exec_va[WATCH_MAX_EXEC];
static volatile LONG s_exec_hits[WATCH_MAX_EXEC];
static int       s_exec_count;
static int       s_length = 4;
static int       s_on_read;          /* RECOMP_WATCH_RW: reads as well as writes */
static int       s_on_exec;          /* RECOMP_WATCH_EXEC: a function entry    */
static volatile LONG s_hits;
static HANDLE    s_watcher;

/* DR7 for DR0: local enable, break on write, and the length encoding the
 * hardware uses (00 = 1 byte, 01 = 2, 11 = 4, 10 = 8). */
static DWORD64 dr7_for_length(int length)
{
    DWORD64 len_bits;
    switch (length) {
    case 1:  len_bits = 0; break;
    case 2:  len_bits = 1; break;
    case 8:  len_bits = 2; break;
    default: len_bits = 3; break;   /* 4 bytes */
    }
    /* R/W0 is 01 for writes and 11 for reads-or-writes. There is no read-only
     * setting on x86: 10 is I/O and 00 is an instruction fetch. So watching
     * reads means watching both, and the report cannot say which it was --
     * good enough for "does anything ever look at this at all". */
    if (s_on_exec) {
        /* L0..L3 are bits 0, 2, 4 and 6. R/W and LEN are left at zero for
         * every slot, which is what encodes "break on execution here". */
        DWORD64 dr7 = 0;
        int i;
        for (i = 0; i < s_exec_count; i++)
            dr7 |= 1ull << (i * 2);
        return dr7;
    }
    return 1ull                    /* L0: enabled for this thread */
         | ((s_on_read ? 3ull : 1ull) << 16)
         | (len_bits << 18);       /* LEN0 */
}

/* A guest address this process can read. The two windows the runtime maps are
 * the low one and the contiguous one; anything else is a stale or wild pointer
 * and dereferencing it would take the title down for the sake of a log line. */
static int guest_readable(uint32_t va)
{
    return (va >= 0x1000u && va < 0x04000000u)
        || (va >= 0x80000000u && va < 0x84000000u);
}

static void arm_thread(DWORD tid)
{
    HANDLE h;
    CONTEXT ctx;

    if (tid == GetCurrentThreadId())
        return;

    h = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                   FALSE, tid);
    if (!h)
        return;

    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;

    if (SuspendThread(h) != (DWORD)-1) {
        DWORD64 want = dr7_for_length(s_length);
        if (GetThreadContext(h, &ctx) && ctx.Dr7 != want) {
            if (s_on_exec) {
                ctx.Dr0 = (DWORD64)(s_exec_count > 0 ? s_exec_addr[0] : 0);
                ctx.Dr1 = (DWORD64)(s_exec_count > 1 ? s_exec_addr[1] : 0);
                ctx.Dr2 = (DWORD64)(s_exec_count > 2 ? s_exec_addr[2] : 0);
                ctx.Dr3 = (DWORD64)(s_exec_count > 3 ? s_exec_addr[3] : 0);
            } else {
                ctx.Dr0 = (DWORD64)s_address;
            }
            ctx.Dr7 = want;
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            SetThreadContext(h, &ctx);
        }
        ResumeThread(h);
    }
    CloseHandle(h);
}

/* Re-arm everything, forever. A title that creates a thread after this started
 * would otherwise write through the watched address unseen, and the write we
 * are looking for is very likely to come from a thread that does not exist
 * yet when the watchpoint is set. */
static DWORD WINAPI watcher_thread(LPVOID unused)
{
    DWORD self = GetCurrentProcessId();
    unsigned passes = 0;

    (void)unused;
    for (;;) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            THREADENTRY32 te;
            te.dwSize = sizeof(te);
            if (Thread32First(snap, &te)) {
                do {
                    if (te.th32OwnerProcessID == self)
                        arm_thread(te.th32ThreadID);
                } while (Thread32Next(snap, &te));
            }
            CloseHandle(snap);
        }

        /* A running total for every armed address, every few seconds.
         *
         * Printing the first N entries and then thinning out answers "did
         * this run at all" and quietly lies about "how many times": a
         * function that ran 41 times and one that ran 400 both show the same
         * last line. That has now produced two wrong readings in this project
         * -- an "exactly 8 ticks" that was really 9, 17 or 41 depending on the
         * run, and a comparison between two counts that were both truncated
         * to the same number. A total costs one line every few seconds and
         * cannot be misread. */
        if (s_on_exec && s_exec_count) {
            static DWORD last;
            DWORD now = GetTickCount();
            if (now - last > 5000) {
                int i;
                last = now;
                fprintf(stderr, "[WATCH] totals:");
                for (i = 0; i < s_exec_count; i++)
                    fprintf(stderr, " 0x%08X=%ld",
                            s_exec_va[i], s_exec_hits[i]);
                fprintf(stderr, "\n");
                fflush(stderr);
            }
        }
        /* Tight at first, then relaxed. Everything this is used to catch in a
         * title that has already settled is caught either way, but a thread
         * created during start-up can enter a loop it never leaves, and then
         * the only chance to arm it is before it gets there. Five seconds of
         * ten-millisecond passes costs nothing and covers the whole of
         * start-up; after that the cadence is back to cheap. */
        Sleep(passes < 500 ? 10 : 200);
        passes++;
    }
}

int watchpoint_init(void)
{
    const char *want = getenv("RECOMP_WATCH_WRITE");
    const char *exec = getenv("RECOMP_WATCH_EXEC");
    const char *len  = getenv("RECOMP_WATCH_LEN");

    if (exec && *exec) {
        const char *cur = exec;
        int i;

        /* A comma-separated list, up to four: "0x69F10,0x686B0,0x181230". */
        while (*cur && s_exec_count < WATCH_MAX_EXEC) {
            uint32_t va = (uint32_t)strtoul(cur, NULL, 0);
            recomp_func_t fn = va ? recomp_lookup(va) : NULL;

            if (!fn)
                fprintf(stderr, "  [WATCH] guest 0x%08X is not a lifted "
                                "function; skipped\n", va);
            else {
                s_exec_va[s_exec_count] = va;
                s_exec_addr[s_exec_count] = (uintptr_t)fn;
                s_exec_count++;
            }
            cur = strchr(cur, ',');
            if (!cur)
                break;
            cur++;
        }
        if (!s_exec_count)
            return 0;

        s_exec_str = getenv("RECOMP_WATCH_EXEC_STR") != NULL;
        {
            const char *st = getenv("RECOMP_WATCH_EXEC_STACK");
            s_exec_stack = (st && *st) ? (int)strtol(st, NULL, 0) : 0;
            st = getenv("RECOMP_WATCH_EXEC_STACK_ENTRIES");
            if (st && *st)
                s_exec_stack_entries = (int)strtol(st, NULL, 0);
            if (s_exec_stack > 512)
                s_exec_stack = 512;
        }
        {
            /* RECOMP_WATCH_EXEC_FIELD reads ecx+offset, which is the
             * ordinary shape of a member read and the one the chain
             * cannot express -- the chain dereferences first, so it
             * answers *(ecx)+off when the question is *(ecx+off). */
            const char *f = getenv("RECOMP_WATCH_EXEC_FIELD");
            s_exec_field = (f && *f) ? (int)strtol(f, NULL, 0) : -1;
        }
        {
            const char *chain = getenv("RECOMP_WATCH_EXEC_CHAIN");
            if (chain && *chain) {
                const char *c = chain;
                while (*c && s_chain_len < WATCH_CHAIN_MAX) {
                    s_chain[s_chain_len++] = (int)strtol(c, NULL, 0);
                    c = strchr(c, ',');
                    if (!c)
                        break;
                    c++;
                }
            }
        }

        s_on_exec = 1;
        s_length = 1;
        s_address = s_exec_addr[0];
        s_watcher = CreateThread(NULL, 0, watcher_thread, NULL, 0, NULL);
        if (!s_watcher) {
            fprintf(stderr, "  [WATCH] could not start the watcher (error %lu)\n",
                    GetLastError());
            return 0;
        }
        for (i = 0; i < s_exec_count; i++)
            fprintf(stderr, "  WATCH: entries to guest 0x%08X (host 0x%p)\n",
                    s_exec_va[i], (void *)s_exec_addr[i]);
        fflush(stderr);
        return 1;
    }

    if (!want || !*want)
        return 0;

    s_guest_va = (uint32_t)strtoul(want, NULL, 0);
    s_on_read = getenv("RECOMP_WATCH_RW") != NULL;
    if (len && *len)
        s_length = (int)strtoul(len, NULL, 0);
    if (s_length != 1 && s_length != 2 && s_length != 4 && s_length != 8)
        s_length = 4;
    if (s_guest_va % (uint32_t)s_length) {
        fprintf(stderr, "  [WATCH] 0x%08X is not %d-byte aligned; "
                        "the hardware cannot watch it\n", s_guest_va, s_length);
        return 0;
    }

    s_address = (uintptr_t)s_guest_va + (uintptr_t)g_xbox_mem_offset;
    s_watcher = CreateThread(NULL, 0, watcher_thread, NULL, 0, NULL);
    if (!s_watcher) {
        fprintf(stderr, "  [WATCH] could not start the watcher (error %lu)\n",
                GetLastError());
        return 0;
    }

    fprintf(stderr, "  WATCH: guest 0x%08X (host 0x%p), %d byte(s), %s\n",
            s_guest_va, (void *)s_address, s_length,
            s_on_read ? "reads and writes" : "writes only");
    fflush(stderr);
    return 1;
}

/* Called from the host's vectored handler for EXCEPTION_SINGLE_STEP. Returns
 * non-zero when this was our watchpoint and execution should continue. */
int watchpoint_handle_trap(void *ctx_ptr)
{
    PCONTEXT ctx = (PCONTEXT)ctx_ptr;
    LONG n;

    if (!s_address || !(ctx->Dr6 & 0xF))
        return 0;

    /* An instruction breakpoint traps before the instruction runs, so the
     * resume flag has to be set or returning here just re-traps on the same
     * byte. There is no value to print either: nothing has been stored, and
     * Dr6 says which of the four addresses it was. */
    if (s_on_exec) {
        DWORD64 which = ctx->Dr6 & 0xF;
        int i;

        ctx->Dr6 = 0;
        ctx->EFlags |= 0x10000;      /* RF */
        for (i = 0; i < s_exec_count; i++) {
            if (!(which & (1ull << i)))
                continue;
            n = InterlockedIncrement(&s_exec_hits[i]);
            /* RECOMP_WATCH_EXEC_ALL prints every entry, for the question "what
             * were the last calls before it went wrong" -- a crash thousands
             * of entries in is invisible to the first-forty budget. */
            static int s_exec_all = -1;
            if (s_exec_all < 0)
                s_exec_all = getenv("RECOMP_WATCH_EXEC_ALL") != NULL;
            /* RECOMP_WATCH_EXEC_FROM=N: print entries N..N+39 instead of the
             * first forty -- a screen minutes into a run is thousands of
             * entries in. */
            static long s_exec_from = -1;
            if (s_exec_from < 0) {
                const char *fr = getenv("RECOMP_WATCH_EXEC_FROM");
                s_exec_from = (fr && *fr) ? strtol(fr, NULL, 0) : 0;
            }
            if ((s_exec_from ? (n >= s_exec_from && n < s_exec_from + 40)
                             : (n <= 40 || n % 500 == 0)) || s_exec_all) {
                /* ecx first: it is the object for a thiscall, and the stack
                 * word above the return address is the first pushed argument
                 * for the rest. Both are guesses about the calling
                 * convention, which is why both are printed rather than one
                 * being chosen. */
                uint32_t arg0 = 0;
                if (g_esp)
                    arg0 = *(volatile uint32_t *)((uintptr_t)(g_esp + 4)
                                                + (uintptr_t)g_xbox_mem_offset);
                fprintf(stderr, "[WATCH] entry #%ld to guest 0x%08X "
                                "(thread %lu) ecx=0x%08X eax=0x%08X "
                                "edx=0x%08X ebx=0x%08X esi=0x%08X edi=0x%08X"
                                " esp=0x%08X arg0=0x%08X\n",
                        n, s_exec_va[i], GetCurrentThreadId(),
                        g_ecx, g_eax, g_edx, g_ebx, g_esi, g_edi,
                        g_esp, arg0);
                /* RECOMP_WATCH_EXEC_FLOATS=off,count with RECOMP_WATCH_EXEC_BASE=
                 * ecx|argN: the floats at base+off. A matrix handed in by
                 * pointer, read at the moment of the call. */
                {
                    static int fl_off = -2, fl_n, fl_arg, fl_deref = -1;
                    if (fl_off == -2) {
                        const char *fs = getenv("RECOMP_WATCH_EXEC_FLOATS");
                        const char *bs = getenv("RECOMP_WATCH_EXEC_BASE");
                        fl_off = -1;
                        if (fs && *fs) {
                            char *e;
                            fl_off = (int)strtol(fs, &e, 0);
                            fl_n = (*e == ',') ? (int)strtol(e + 1, NULL, 0) : 6;
                            if (fl_n > 16) fl_n = 16;
                        }
                        fl_arg = (bs && bs[0] == 'a') ? (int)strtol(bs + 3, NULL, 0) : -1;
                        /* deref:<off> -- the pointer stored at ecx+off */
                        if (bs && bs[0] == 'd')
                            fl_deref = (int)strtol(bs + 6, NULL, 0);
                    }
                    if (fl_off >= 0 && g_esp) {
                        uint32_t base = fl_arg < 0 ? g_ecx
                            : *(volatile uint32_t *)((uintptr_t)(g_esp + 4 + 4u * (uint32_t)fl_arg)
                                                     + (uintptr_t)g_xbox_mem_offset);
                        if (fl_deref >= 0 && guest_readable(g_ecx + (uint32_t)fl_deref))
                            base = *(volatile uint32_t *)((uintptr_t)(g_ecx + (uint32_t)fl_deref)
                                                          + (uintptr_t)g_xbox_mem_offset);
                        uint32_t at = base + (uint32_t)fl_off;
                        if (guest_readable(at) && guest_readable(at + 4u * (uint32_t)fl_n - 4u)) {
                            int k;
                            fprintf(stderr, "          [0x%08X+0x%X]", base, (unsigned)fl_off);
                            for (k = 0; k < fl_n; k++) {
                                float v;
                                uint32_t w = *(volatile uint32_t *)((uintptr_t)(at + 4u * (uint32_t)k)
                                                                    + (uintptr_t)g_xbox_mem_offset);
                                memcpy(&v, &w, 4);
                                if (getenv("RECOMP_WATCH_EXEC_FLOATS_HEX"))
                                    fprintf(stderr, " %08X", w);   /* pointers and names too */
                                else
                                    fprintf(stderr, " %g", v);
                            }
                            fputc('\n', stderr);
                        }
                    }
                }
                if (s_exec_field >= 0 && guest_readable(g_ecx)) {
                    uint32_t at = g_ecx + (uint32_t)s_exec_field;
                    if (guest_readable(at))
                        fprintf(stderr, "          [ecx+0x%X] = 0x%08X\n",
                                s_exec_field,
                                *(volatile uint32_t *)((uintptr_t)at
                                        + (uintptr_t)g_xbox_mem_offset));
                }
                if (s_exec_str) {
                    static const char *const what[2] = { "ecx", "arg0" };
                    uint32_t from[2];
                    int w;

                    from[0] = g_ecx;
                    from[1] = arg0;
                    for (w = 0; w < 2; w++) {
                        char text[48];
                        int len = 0;
                        if (!guest_readable(from[w]))
                            continue;
                        while (len < (int)sizeof text - 1) {
                            unsigned char c =
                                *(volatile unsigned char *)((uintptr_t)(from[w] + (uint32_t)len)
                                                          + (uintptr_t)g_xbox_mem_offset);
                            if (c == 0)
                                break;
                            if (c < 0x20 || c > 0x7E) { len = 0; break; }
                            text[len++] = (char)c;
                        }
                        if (len > 1) {
                            text[len] = 0;
                            fprintf(stderr, "          %s -> \"%s\"\n",
                                    what[w], text);
                        }
                    }
                }
                if (s_exec_stack > 0 && n <= s_exec_stack_entries) {
                    int k;
                    for (k = 0; k < s_exec_stack; k++) {
                        uint32_t at = g_esp + 4u * (uint32_t)k;
                        uint32_t v;
                        if (!guest_readable(at))
                            break;
                        v = *(volatile uint32_t *)((uintptr_t)at
                                                   + (uintptr_t)g_xbox_mem_offset);
                        /* The first eight words whatever they hold -- they
                         * are the return address and the arguments -- and
                         * past that only words that could be a code address;
                         * the filter in guest-stack.py does the real test. */
                        if (k < 8 || (v >= 0x00010000u && v < 0x00400000u))
                            fprintf(stderr, "    GS +%d %08X\n", k * 4, v);
                    }
                }
                if (s_chain_len) {
                    uint32_t cur = g_ecx;
                    int k;
                    for (k = 0; k < s_chain_len; k++) {
                        uint32_t next;
                        if (!guest_readable(cur)) {
                            fprintf(stderr, "          chain[%d]: 0x%08X is "
                                            "not readable; stopping\n",
                                    k, cur);
                            break;
                        }
                        next = *(volatile uint32_t *)((uintptr_t)cur
                                        + (uintptr_t)g_xbox_mem_offset);
                        next += (uint32_t)s_chain[k];
                        fprintf(stderr, "          chain[%d]: *(0x%08X) + %d "
                                        "= 0x%08X\n",
                                k, cur, s_chain[k], next);
                        cur = next;
                    }
                }
            }
        }
        fflush(stderr);
        return 1;
    }

    ctx->Dr6 = 0;                    /* the handler acknowledges the hit */
    n = InterlockedIncrement(&s_hits);

    /* The instruction has already completed when the trap arrives, so the
     * value printed is the one that was just stored -- which is the whole
     * point: a write of zero and a write of a pointer are the difference
     * between a reset and the thing being looked for. */
    fprintf(stderr, "[WATCH] hit #%ld: guest 0x%08X now 0x%08X, touched from "
                    "host RIP 0x%llX (thread %lu)\n",
            n, s_guest_va, *(volatile uint32_t *)s_address,
            (unsigned long long)ctx->Rip, GetCurrentThreadId());

    /* The guest stack as well as the host instruction.
     *
     * Naming the instruction that wrote is only half the answer when the
     * write is correct in itself and the pointer it was handed is not. The
     * store that corrupted the thread block here is `if (out) *out = n` --
     * an ordinary optional out-parameter, faultless code -- and everything
     * worth knowing is in who passed it 0x1020. That is on the stack at the
     * moment of the write and nowhere else afterwards, so it is printed
     * here rather than reconstructed later.
     *
     * A scan, not a walk, in the same sense as the crash handler's: these
     * are stack words that look like return addresses, so some are stale.
     * scripts/guest-stack.py filters the same shape against the real call
     * sites. */
    /* The guest registers as well. When the faulty write is a copy loop
     * the destination pointer is in a register, and the register says
     * directly what the stack only implies. */
    fprintf(stderr, "        ecx=0x%08X edx=0x%08X eax=0x%08X esi=0x%08X"
                    " edi=0x%08X\n", g_ecx, g_edx, g_eax, g_esi, g_edi);

    if (g_esp) {
        uint32_t sp = g_esp;
        int printed = 0, k;

        fprintf(stderr, "        guest esp 0x%08X, stack words:\n", sp);
        for (k = 0; k < 64 && printed < 8; k++) {
            uint32_t slot = sp + (uint32_t)(k * 4);
            uint32_t v;
            if (slot < 0x1000u || slot >= 0x04000000u)
                break;
            v = *(volatile uint32_t *)((uintptr_t)slot
                                     + (uintptr_t)g_xbox_mem_offset);
            /* Code addresses in this image, which is where the callers are. */
            if (v >= 0x00010000u && v < 0x00400000u) {
                fprintf(stderr, "          [esp+%-3d] 0x%08X\n", k * 4, v);
                printed++;
            }
        }
    }
    fflush(stderr);
    return 1;
}
