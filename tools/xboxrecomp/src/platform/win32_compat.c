/*
 * win32_compat.c - POSIX implementation of generic Win32 host primitives.
 *
 * See win32_compat.h. This is deliberately a *generic* OS-primitive layer
 * (threads/events/mutexes/atomics/heap/timers) -- it carries no Xbox
 * semantics. The Xbox kernel HLE in src/kernel builds on top of it.
 *
 * POSIX (Linux/MacOS) only.
 */

#if !defined(_WIN32)

/* Enable memfd_create, MAP_FIXED_NOREPLACE, timegm. Must precede all #includes. */
#define _GNU_SOURCE
/* Darwin: exposes memset_s, its explicit_bzero equivalent. */
#define __STDC_WANT_LIB_EXT1__ 1

#include "win32_compat.h"
#include "mmio_trap.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>
#include <fenv.h>
#include <sys/mman.h>
#if defined(__ANDROID__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <mach/thread_switch.h>
#include <pthread/qos.h>
#else
#include <sys/sysinfo.h>
#endif

/* ===================================================================== */
/* Last-error (thread-local)                                             */
/* ===================================================================== */

static __thread DWORD t_last_error = 0;

DWORD GetLastError(void)            { return t_last_error; }
VOID  SetLastError(DWORD code)      { t_last_error = code; }

/* ===================================================================== */
/* One guest CPU without thread affinity                                 */
/* ===================================================================== */

/*
 * A console title was written for one core, and some rely on it without
 * saying so: two of its threads never run at the same instant. Where the
 * host can pin threads, the kernel layer puts every thread that runs guest
 * code on one CPU. Darwin on Apple Silicon cannot, so the same rule is kept
 * here with a token: a thread that has joined runs only while it holds it.
 *
 *   - Every blocking primitive in this file gives the token up for as long
 *     as it blocks, and takes it back before returning.
 *   - A thread that has waited a millisecond for it interrupts the holder
 *     with a signal. A holder that was in guest code hands it over from the
 *     handler: that is the preemption a single core gives the title, and a
 *     thread spinning on a flag another thread sets still lets that thread
 *     run. A holder that was in host code may hold a lock the next thread
 *     needs (the C library's, a device model's), so it only notes the
 *     request and hands over at guest_turn_checkpoint(), which the kernel
 *     layer calls on the way back to the title. What counts as guest code is
 *     the range the program gives guest_turn_set_code(); with none, every
 *     hand-over waits for a checkpoint or a blocking call. A
 *     thread coming out of a wait, and one at time-critical priority (the
 *     kernel layer's interrupt thread), does not wait the millisecond.
 *   - Waiters are served in arrival order, so a thread that hands over is
 *     behind every thread that was already waiting.
 *
 * A thread that has not joined is never held up and never signalled.
 */
#define GC_SIGNAL    SIGUSR2
#define GC_SLICE_NS  1000000L

typedef struct gc_thread {
    int holding;                    /* has the token */
    int urgent;                     /* interrupts the holder at once */
    volatile sig_atomic_t pending;  /* asked to hand over while in host code */
    volatile uintptr_t asked_pc;    /* where it was when first asked ... */
    struct timespec asked_at;       /* ... and when */
    volatile sig_atomic_t busy;     /* inside the token's own code */
    void *obj;                      /* its w32_object, for its priority */
    int via_urgent;                 /* holds the CPU outside the ticket order */
    int yielding;                   /* gave the CPU up on purpose: queue behind the rest */
    /* What guest_turn_probe() reports: set by the thread itself, read under gc_lock. */
    int state;                      /* GC_OFF, GC_TURN, GC_RUN */
    struct timespec state_at;       /* when it entered that state */
    pthread_t self;
    const void *start;              /* the host entry of the guest routine it runs */
    uint32_t start_ctx;
    const volatile uint32_t *caller;    /* its g_xbox_kernel_caller */
    int gone;                       /* the thread has ended; kept so a probe never reads freed memory */
    struct gc_thread *next;         /* every thread that has joined */
} gc_thread;

enum { GC_OFF, GC_TURN, GC_RUN };

static pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  gc_cond = PTHREAD_COND_INITIALIZER;
static uint64_t  gc_next, gc_serving;   /* tickets: issued, allowed to run */
static pthread_t gc_holder;
static int       gc_held;
static volatile uint64_t gc_handoffs, gc_kicks;
static uintptr_t gc_code_lo, gc_code_hi;
static volatile long gc_longest_wait_ms;    /* by a thread due to run at once */
/* RECOMP_GUEST_TURN_PRIO (default 1): a thread the title made time-critical is served
 * before the threads queued in arrival order, as a higher priority runs first on the
 * console. Def Jam's audio writer, which refills the 50 ms ring the voice processor plays,
 * otherwise waited 24-52 ms behind other threads while a Story scene loaded. 0 restores
 * plain arrival order. */
static int       gc_prio = -1;
static int       gc_urgent_waiting;         /* urgent threads waiting for the CPU */
static pthread_key_t  gc_key;
static pthread_once_t gc_once = PTHREAD_ONCE_INIT;
static __thread gc_thread *t_gc;
static gc_thread *gc_all;                   /* under gc_lock */

static void gc_set_state(gc_thread *t, int state)
{
    t->state = state;
    clock_gettime(CLOCK_MONOTONIC, &t->state_at);
}

static void gc_release(gc_thread *t)
{
    t->busy = 1;
    pthread_mutex_lock(&gc_lock);
    t->holding = 0;
    gc_held = 0;
    gc_set_state(t, GC_OFF);
    if (t->via_urgent)
        t->via_urgent = 0;          /* took no ticket */
    else
        gc_serving++;
    pthread_cond_broadcast(&gc_cond);
    pthread_mutex_unlock(&gc_lock);
    t->busy = 0;
}

static void gc_acquire(gc_thread *t, int waited)
{
    uint64_t me = 0;
    struct timespec t0, t1;
    int timed, urgent;

    t->busy = 1;
    if (gc_prio < 0) {
        const char *e = getenv("RECOMP_GUEST_TURN_PRIO");
        gc_prio = !(e && *e == '0');
    }
    urgent = gc_prio && t->urgent && !t->yielding;
    if (urgent)
        waited = 1;
    timed = waited;         /* a thread that should have run at once */
    if (timed)
        clock_gettime(CLOCK_MONOTONIC, &t0);
    pthread_mutex_lock(&gc_lock);
    gc_set_state(t, GC_TURN);
    if (urgent)
        gc_urgent_waiting++;
    else
        me = gc_next++;
    while (urgent ? gc_held : (gc_serving != me || gc_held || gc_urgent_waiting)) {
        struct timespec ts;
        /* The holder cannot exit without this lock, so it is there to be
         * signalled. A signal that finds it between two holds is lost, which
         * the next pass makes good. */
        if (waited && gc_held) {
            pthread_kill(gc_holder, GC_SIGNAL);
            gc_kicks++;
        }
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += GC_SLICE_NS;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        if (pthread_cond_timedwait(&gc_cond, &gc_lock, &ts) == ETIMEDOUT)
            waited = 1;
    }
    if (urgent) {
        gc_urgent_waiting--;
        t->via_urgent = 1;
    }
    gc_holder = pthread_self();
    gc_held = 1;
    t->holding = 1;
    gc_set_state(t, GC_RUN);
    pthread_mutex_unlock(&gc_lock);
    if (timed) {
        long ms;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (long)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (ms > gc_longest_wait_ms)
            gc_longest_wait_ms = ms;
    }
    t->busy = 0;
}

/* The holder was interrupted because another thread has waited its slice. */
static void gc_preempt(int sig, siginfo_t *si, void *uctx)
{
    gc_thread *t = t_gc;
    int e = errno;

    (void)sig; (void)si;
    if (t && t->holding && !t->busy) {
        uintptr_t pc = mmio_trap_pc(uctx);
        if (pc >= gc_code_lo && pc < gc_code_hi) {
            gc_handoffs++;
            t->pending = 0;
            gc_release(t);
            gc_acquire(t, t->urgent);
        } else if (!t->pending) {
            t->asked_pc = pc;
            clock_gettime(CLOCK_MONOTONIC, &t->asked_at);
            t->pending = 1;
        }
    }
    errno = e;
}

static void gc_bind_self(gc_thread *t);
static void gc_unbind(gc_thread *t);

static void gc_thread_gone(void *p)
{
    gc_thread *t = p;

    if (t->holding)
        gc_release(t);
    gc_unbind(t);
    pthread_mutex_lock(&gc_lock);
    t->gone = 1;                    /* stays on gc_all: a probe may hold a pointer to it */
    t->caller = NULL;
    pthread_mutex_unlock(&gc_lock);
}

static void gc_setup(void)
{
    struct sigaction sa;

    pthread_key_create(&gc_key, gc_thread_gone);
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = gc_preempt;
    sa.sa_flags = SA_RESTART | SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(GC_SIGNAL, &sa, NULL);
}

void guest_turn_join(void)
{
    gc_thread *t = t_gc;

    if (t)
        return;
    pthread_once(&gc_once, gc_setup);
    t = calloc(1, sizeof *t);
    if (!t)
        return;
    pthread_setspecific(gc_key, t);
    t_gc = t;
    t->self = pthread_self();
    gc_bind_self(t);
    pthread_mutex_lock(&gc_lock);
    t->next = gc_all;
    gc_all = t;
    pthread_mutex_unlock(&gc_lock);
#if defined(__APPLE__)
    /* The default QoS left a holder off the CPU for tens of milliseconds at a time
     * (0.2 ms of CPU in 48) while a busy host thread logged or a scene loaded,
     * and every guest thread, the time-critical audio writer among them, waited
     * behind it: the audio gaps at Story scene changes. A guest thread holds the
     * console's one CPU, so it runs at the QoS the writer already has.
     * RECOMP_GUEST_QOS=0 leaves the default. */
    {
        static int qos = -1;
        if (qos < 0) {
            const char *e = getenv("RECOMP_GUEST_QOS");
            qos = !(e && *e == '0');
        }
        if (qos)
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
#endif
    gc_acquire(t, 0);
}

void guest_turn_set_code(const void *start, size_t size)
{
    gc_code_lo = (uintptr_t)start;
    gc_code_hi = (uintptr_t)start + size;
}

static void gc_note_late(gc_thread *t);

void guest_turn_checkpoint(void)
{
    gc_thread *t = t_gc;

    if (t && t->pending) {
        t->pending = 0;
        gc_note_late(t);
        if (t->holding) {
            gc_handoffs++;
            gc_release(t);
            gc_acquire(t, t->urgent);
        }
    }
}

long guest_turn_longest_wait_ms(void)
{
    long ms = gc_longest_wait_ms;

    gc_longest_wait_ms = 0;
    return ms;
}

void guest_turn_counts(uint64_t *handoffs, uint64_t *kicks)
{
    if (handoffs) *handoffs = gc_handoffs;
    if (kicks)    *kicks = gc_kicks;
}

/* A thread kept the guest CPU in host code long after it was asked for it.
 * Nothing is wrong with a few milliseconds; tens are a hitch the title was
 * never given on its console, and the address says which host function to
 * teach to give the CPU up. */
static void gc_note_late(gc_thread *t)
{
    struct timespec now;
    long ms;
    static volatile LONG lines;

    clock_gettime(CLOCK_MONOTONIC, &now);
    ms = (long)(now.tv_sec - t->asked_at.tv_sec) * 1000 +
         (now.tv_nsec - t->asked_at.tv_nsec) / 1000000;
    if (ms >= 10 && InterlockedIncrement(&lines) <= 40)
        fprintf(stderr, "  [KERNEL] guest CPU kept %ld ms in host code after it was"
                        " asked for (at host pc %p)\n", ms, (void *)t->asked_pc);
}

/* Around anything that blocks: gc_block() before, gc_unblock(its result)
 * after. Nothing happens on a thread that has not joined. */
static int gc_block(void)
{
    gc_thread *t = t_gc;

    if (!t || !t->holding)
        return 0;
    if (t->pending) {
        t->pending = 0;
        gc_note_late(t);
    }
    gc_release(t);
    return 1;
}

static void gc_unblock(int blocked)
{
    /* A thread that wakes runs at once, as the priority boost a wait ends
     * with gives it on the console's kernel; only threads that were
     * themselves preempted, or yielded, wait out a slice. */
    if (blocked)
        gc_acquire(t_gc, 1);
}

int guest_turn_leave(void)       { return gc_block(); }
void guest_turn_return(int left) { gc_unblock(left); }

/* Let a waiting thread run, as a yield on one core does. */
static void gc_yield(void)
{
    gc_thread *t = t_gc;
    int waiting;

    if (!t || !t->holding)
        return;
    t->busy = 1;
    pthread_mutex_lock(&gc_lock);
    /* A time-critical thread waits outside the tickets, and a holder that came in
     * that way took none of its own. Counting tickets alone, a thread spinning on
     * SwitchToThread in host code (Direct3D's fence hold) kept a waiting
     * time-critical thread out for up to 100 ms. */
    waiting = gc_urgent_waiting || gc_next != gc_serving + !t->via_urgent;
    pthread_mutex_unlock(&gc_lock);
    t->busy = 0;
    if (waiting) {
        t->pending = 0;
        gc_release(t);
        t->yielding = 1;
        gc_acquire(t, t->urgent);
        t->yielding = 0;
    } else {
        t->pending = 0;     /* whoever asked has been and gone */
    }
}

/* ===================================================================== */
/* Hitch probe: who holds the guest CPU, and where every thread is       */
/* ===================================================================== */

/*
 * A stall of 100-300 ms once a minute does not show in an aggregate profile.
 * The play-test recorder calls guest_turn_probe() when no frame has been
 * presented for a while, and keeps what it says with the hitch: which thread
 * holds the guest CPU, how long it has, the guest call site of the kernel call
 * it is in (g_xbox_kernel_caller), the last indirect calls, and the native
 * stack of that thread and of every other thread in the process.
 *
 * Darwin only (the other hosts pin guest threads to a CPU and have no token).
 * Threads are sampled from outside with Mach, one at a time: suspended, their
 * registers read and their frame-pointer chain copied with vm_read (which
 * cannot fault), resumed. Nothing that takes a lock runs while one is
 * suspended; names are looked up afterwards with dladdr. Frames in the
 * program itself also carry their offset from its load address ("exe+0x..."),
 * because dladdr names only exported symbols: scripts/hitch-probes.py turns
 * them into functions and lines with atos.
 */
static const volatile uint32_t *gc_icall_ring, *gc_icall_idx;
static const char *gc_extra_key;
static int (*gc_extra_fn)(char *out, size_t size);

void guest_turn_note_extra(const char *key, int (*fn)(char *out, size_t size))
{
    gc_extra_key = key;
    gc_extra_fn = fn;
}

void guest_turn_note_caller(const volatile uint32_t *slot)
{
    if (t_gc)
        t_gc->caller = slot;
}

void guest_turn_note_start(const void *entry, uint32_t ctx)
{
    if (t_gc) {
        t_gc->start = entry;
        t_gc->start_ctx = ctx;
    }
}

void guest_turn_note_icall_ring(const volatile uint32_t *ring, const volatile uint32_t *idx)
{
    gc_icall_ring = ring;
    gc_icall_idx = idx;
}

#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/dyld.h>

#define PROBE_THREADS 96
#define PROBE_FRAMES  40

typedef struct {
    thread_act_t port;
    char name[48];
    int gc;                         /* -1: not a guest thread; else GC_OFF/TURN/RUN */
    int holder, pending;
    long state_ms, asked_ms;
    uint32_t caller;
    const void *start;
    uint32_t start_ctx;
    int run_state;
    long cpu_ms;
    int pri, depressed;
    int nframes;
    uintptr_t pc[PROBE_FRAMES];
} probe_thread;

typedef struct { char *p; size_t left; } jbuf;

static void jput(jbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void jput(jbuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    if (b->left <= 1)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->p, b->left, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= b->left)
        n = (int)b->left - 1;
    b->p += n;
    b->left -= (size_t)n;
}

static int probe_read(uintptr_t addr, void *out, size_t n)
{
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)addr, n,
                                  (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}

static long ms_since(const struct timespec *a, const struct timespec *now)
{
    return (long)(now->tv_sec - a->tv_sec) * 1000 + (now->tv_nsec - a->tv_nsec) / 1000000;
}

/* Registers and frame-pointer chain of a suspended thread. */
static int probe_walk(thread_act_t th, uintptr_t *pcs, int max)
{
    const uintptr_t mask = ((uintptr_t)1 << 47) - 1;   /* user addresses; drops any pointer signature */
    uintptr_t fp, frame[2];
    int n = 0;
#if defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t st;
    mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(th, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS)
        return 0;
    pcs[n++] = (uintptr_t)arm_thread_state64_get_pc(st) & mask;
    /* A leaf (a system call's stub) has not saved its return address yet. */
    pcs[n++] = (uintptr_t)arm_thread_state64_get_lr(st) & mask;
    fp = (uintptr_t)arm_thread_state64_get_fp(st);
#elif defined(__x86_64__)
    x86_thread_state64_t st;
    mach_msg_type_number_t cnt = x86_THREAD_STATE64_COUNT;
    if (thread_get_state(th, x86_THREAD_STATE64, (thread_state_t)&st, &cnt) != KERN_SUCCESS)
        return 0;
    pcs[n++] = (uintptr_t)st.__rip;
    fp = (uintptr_t)st.__rbp;
#else
    return 0;
#endif
    while (n < max && fp && !(fp & 7) && probe_read(fp, frame, sizeof frame)) {
        uintptr_t ret = frame[1] & mask;
        if (!ret)
            break;
        if (!(n == 2 && ret == pcs[1]))
            pcs[n++] = ret;
        if (frame[0] <= fp)
            break;
        fp = frame[0];
    }
    return n;
}

/* dladdr scans a symbol table, and a stall's stacks repeat from one probe to the
 * next: names are kept by address (under the probe's lock). */
#define PROBE_NAMES 4096
static struct { uintptr_t pc; char s[176]; } probe_names[PROBE_NAMES];

static void probe_frame_name(jbuf *b, uintptr_t pc, uintptr_t exe)
{
    Dl_info di;
    const char *lib;

    if (dladdr((const void *)pc, &di) && di.dli_fbase) {
        lib = di.dli_fname ? strrchr(di.dli_fname, '/') : NULL;
        lib = lib ? lib + 1 : (di.dli_fname ? di.dli_fname : "?");
        if ((uintptr_t)di.dli_fbase == exe) {
            if (di.dli_sname)
                jput(b, "\"%s+0x%lx exe+0x%lx\"", di.dli_sname,
                     (unsigned long)(pc - (uintptr_t)di.dli_saddr), (unsigned long)(pc - exe));
            else
                jput(b, "\"exe+0x%lx\"", (unsigned long)(pc - exe));
        } else if (di.dli_sname) {
            jput(b, "\"%s`%s+0x%lx\"", lib, di.dli_sname, (unsigned long)(pc - (uintptr_t)di.dli_saddr));
        } else {
            jput(b, "\"%s+0x%lx\"", lib, (unsigned long)(pc - (uintptr_t)di.dli_fbase));
        }
    } else {
        jput(b, "\"0x%lx\"", (unsigned long)pc);
    }
}

static void probe_frame(jbuf *b, uintptr_t pc, uintptr_t exe)
{
    size_t h = (size_t)((pc >> 2) * 0x9E3779B97F4A7C15ull >> 52) & (PROBE_NAMES - 1);
    jbuf nb;

    if (!pc) {
        jput(b, "\"0x0\"");
        return;
    }
    if (probe_names[h].pc != pc) {
        nb.p = probe_names[h].s;
        nb.left = sizeof probe_names[h].s;
        probe_frame_name(&nb, pc, exe);
        if (nb.left <= 1)       /* cut short: close the string */
            memcpy(probe_names[h].s + sizeof probe_names[h].s - 2, "\"", 2);
        probe_names[h].pc = pc;
    }
    jput(b, "%s", probe_names[h].s);
}

static const char *probe_run_state(int s)
{
    switch (s) {
    case TH_STATE_RUNNING:         return "running";
    case TH_STATE_STOPPED:         return "stopped";
    case TH_STATE_WAITING:         return "waiting";
    case TH_STATE_UNINTERRUPTIBLE: return "uninterruptible";
    case TH_STATE_HALTED:          return "halted";
    default:                       return "?";
    }
}

static void probe_thread_json(jbuf *b, const probe_thread *p, int frames, uintptr_t exe, int icall)
{
    static const char *gcs[] = { "off", "turn", "run" };
    Dl_info di;
    int i;

    jput(b, "{\"name\":\"%s\"", p->name);
    if (p->gc >= 0) {
        jput(b, ",\"guest\":\"%s\",\"state_ms\":%ld", gcs[p->gc], p->state_ms);
        if (p->holder)
            jput(b, ",\"holder\":true");
        if (p->pending)
            jput(b, ",\"asked_ms\":%ld", p->asked_ms);
        if (p->caller)
            jput(b, ",\"kernel_caller\":\"0x%08X\"", p->caller);
        if (p->start) {
            if (dladdr(p->start, &di) && di.dli_sname)
                jput(b, ",\"start\":\"%s\"", di.dli_sname);
            else
                jput(b, ",\"start\":\"exe+0x%lx\"", (unsigned long)((uintptr_t)p->start - exe));
            jput(b, ",\"start_ctx\":\"0x%08X\"", p->start_ctx);
        }
        if (icall && gc_icall_ring && gc_icall_idx) {
            /* The ring is every thread's; with one guest CPU its newest entries are the holder's. */
            uint32_t idx = *gc_icall_idx;
            jput(b, ",\"icall\":[");
            for (i = 0; i < 16; i++)
                jput(b, "%s\"0x%08X\"", i ? "," : "", gc_icall_ring[(idx - 1 - (uint32_t)i) & 15]);
            jput(b, "]");
        }
    }
    jput(b, ",\"run\":\"%s\",\"cpu_ms\":%ld,\"pri\":%d%s,\"stack\":[", probe_run_state(p->run_state),
         p->cpu_ms, p->pri, p->depressed ? ",\"depressed\":true" : "");
    for (i = 0; i < p->nframes && i < frames; i++) {
        if (i)
            jput(b, ",");
        probe_frame(b, p->pc[i], exe);
    }
    jput(b, "]}");
}

int guest_turn_probe(char *holder, size_t holder_size, char *all, size_t all_size)
{
    static pthread_mutex_t probe_lock = PTHREAD_MUTEX_INITIALIZER;
    static probe_thread th[PROBE_THREADS];
    thread_act_array_t list = NULL;
    mach_msg_type_number_t count = 0, i;
    thread_act_t me = mach_thread_self();
    struct timespec now, t0, t1;
    static char extra[512];
    uintptr_t exe = (uintptr_t)_dyld_get_image_header(0);
    int n = 0, k, held, holder_i = -1;
    pthread_t holder_pt;
    jbuf b;

    if (holder && holder_size) holder[0] = 0;
    if (all && all_size) all[0] = 0;
    pthread_mutex_lock(&probe_lock);
    if (task_threads(mach_task_self(), &list, &count) != KERN_SUCCESS) {
        pthread_mutex_unlock(&probe_lock);
        mach_port_deallocate(mach_task_self(), me);
        return 0;
    }
    /* Names and guest-CPU state first, while no thread is stopped. */
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&gc_lock);
    held = gc_held;
    holder_pt = gc_holder;
    pthread_mutex_unlock(&gc_lock);
    for (i = 0; i < count && n < PROBE_THREADS; i++) {
        probe_thread *p = &th[n];
        pthread_t pt;
        gc_thread *g;

        if (list[i] == me)
            continue;
        memset(p, 0, sizeof *p);
        p->port = list[i];
        p->gc = -1;
        pt = pthread_from_mach_thread_np(list[i]);
        if (pt) {
            pthread_getname_np(pt, p->name, sizeof p->name);
            for (k = 0; p->name[k]; k++)
                if (p->name[k] == '"' || p->name[k] == '\\' || (unsigned char)p->name[k] < 32)
                    p->name[k] = '_';
            pthread_mutex_lock(&gc_lock);
            for (g = gc_all; g; g = g->next) {
                if (g->gone || !pthread_equal(g->self, pt))
                    continue;
                p->gc = g->state;
                p->state_ms = ms_since(&g->state_at, &now);
                p->holder = held && pthread_equal(holder_pt, pt);
                p->pending = g->pending;
                if (g->pending)
                    p->asked_ms = ms_since(&g->asked_at, &now);
                p->start = g->start;
                p->start_ctx = g->start_ctx;
                if (g->caller)
                    probe_read((uintptr_t)g->caller, &p->caller, sizeof p->caller);
                break;
            }
            pthread_mutex_unlock(&gc_lock);
        }
        if (p->holder)
            holder_i = n;
        n++;
    }
    /* Then each thread's stack, one stopped at a time. */
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (k = 0; k < n; k++) {
        probe_thread *p = &th[k];
        thread_basic_info_data_t bi;
        mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;

        if (thread_info(p->port, THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) == KERN_SUCCESS) {
            p->run_state = bi.run_state;
            p->cpu_ms = (long)(bi.user_time.seconds + bi.system_time.seconds) * 1000 +
                        (bi.user_time.microseconds + bi.system_time.microseconds) / 1000;
        }
        {
            policy_timeshare_info_data_t ts;
            mach_msg_type_number_t tc = POLICY_TIMESHARE_INFO_COUNT;
            if (thread_info(p->port, THREAD_SCHED_TIMESHARE_INFO, (thread_info_t)&ts, &tc) == KERN_SUCCESS) {
                p->pri = ts.cur_priority;
                p->depressed = ts.depressed;
            } else {
                p->pri = -1;
            }
        }
        if (thread_suspend(p->port) == KERN_SUCCESS) {
            p->nframes = probe_walk(p->port, p->pc, PROBE_FRAMES);
            thread_resume(p->port);
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    for (i = 0; i < count; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, count * sizeof *list);
    mach_port_deallocate(mach_task_self(), me);

    extra[0] = 0;
    if (gc_extra_fn && gc_extra_key && gc_extra_fn(extra, sizeof extra) <= 0)
        extra[0] = 0;
    if (holder && holder_size) {
        b.p = holder; b.left = holder_size;
        if (holder_i >= 0) {
            probe_thread_json(&b, &th[holder_i], PROBE_FRAMES, exe, 1);
        } else {
            jput(&b, "{\"name\":null,\"held\":%d}", held);
        }
        if (extra[0] && b.p > holder && b.p[-1] == '}') {
            b.p--; b.left++;            /* reopen the object for the extra */
            jput(&b, ",\"%s\":%s}", gc_extra_key, extra);
        }
    }
    if (all && all_size) {
        b.p = all; b.left = all_size;
        jput(&b, "{\"exe\":\"0x%lx\",\"sample_us\":%ld,", (unsigned long)exe,
             (long)(t1.tv_sec - t0.tv_sec) * 1000000 + (t1.tv_nsec - t0.tv_nsec) / 1000);
        if (extra[0])
            jput(&b, "\"%s\":%s,", gc_extra_key, extra);
        jput(&b, "\"threads\":[");
        for (k = 0; k < n; k++) {
            if (k)
                jput(&b, ",");
            probe_thread_json(&b, &th[k], PROBE_FRAMES, exe, 0);
        }
        jput(&b, "]}");
    }
    pthread_mutex_unlock(&probe_lock);
    return n;
}
#else
int guest_turn_probe(char *holder, size_t holder_size, char *all, size_t all_size)
{
    if (holder && holder_size) holder[0] = 0;
    if (all && all_size) all[0] = 0;
    return 0;
}
#endif

/* ===================================================================== */
/* Interlocked atomics                                                   */
/* ===================================================================== */

LONG InterlockedIncrement(volatile LONG *p)        { return __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedDecrement(volatile LONG *p)        { return __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST); }
LONG InterlockedExchange(volatile LONG *p, LONG v) { return __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST); }
LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST); }

LONG InterlockedCompareExchange(volatile LONG *p, LONG xchg, LONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p, LONGLONG xchg, LONGLONG cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

PVOID InterlockedCompareExchangePointer(PVOID volatile *p, PVOID xchg, PVOID cmp)
{
    __atomic_compare_exchange_n(p, &cmp, xchg, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    return cmp;
}

/* ===================================================================== */
/* Critical sections (recursive pthread mutex)                           */
/* ===================================================================== */

VOID InitializeCriticalSection(LPCRITICAL_SECTION cs)
{
    memset(cs, 0, sizeof(*cs));
    pthread_mutex_t *m = (pthread_mutex_t *)malloc(sizeof(pthread_mutex_t));
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->LockSemaphore = m;
}

VOID InitializeCriticalSectionAndSpinCount(LPCRITICAL_SECTION cs, DWORD spin)
{
    InitializeCriticalSection(cs);
    cs->SpinCount = spin;
}

VOID EnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) != 0) {
        /* Its owner may need the guest CPU to get as far as leaving. */
        int blocked = gc_block();
        pthread_mutex_lock((pthread_mutex_t *)cs->LockSemaphore);
        gc_unblock(blocked);
    }
    cs->RecursionCount++;
}

VOID LeaveCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) return;
    cs->RecursionCount--;
    pthread_mutex_unlock((pthread_mutex_t *)cs->LockSemaphore);
}

BOOL TryEnterCriticalSection(LPCRITICAL_SECTION cs)
{
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    if (pthread_mutex_trylock((pthread_mutex_t *)cs->LockSemaphore) == 0) {
        cs->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

VOID DeleteCriticalSection(LPCRITICAL_SECTION cs)
{
    if (cs->LockSemaphore) {
        pthread_mutex_destroy((pthread_mutex_t *)cs->LockSemaphore);
        free(cs->LockSemaphore);
        cs->LockSemaphore = NULL;
    }
}

/* ===================================================================== */
/* Slim reader/writer locks                                              */
/* ===================================================================== */

/* An SRWLOCK is usable straight from SRWLOCK_INIT, so the pthread_rwlock_t
 * behind it has to appear on first use. Unlike the condition variables below
 * -- whose lazy init is covered by the caller holding the paired CRITICAL
 * SECTION -- an SRWLOCK is by definition taken from several threads at once
 * with nothing else held, so first use genuinely races. Serialise just that:
 * once Ptr is published, every acquire is a plain atomic load. */
static pthread_rwlock_t *srw_lazy_init(PSRWLOCK lock)
{
    pthread_rwlock_t *rw = __atomic_load_n((pthread_rwlock_t **)&lock->Ptr,
                                           __ATOMIC_ACQUIRE);
    if (!rw) {
        static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&init_lock);
        rw = (pthread_rwlock_t *)lock->Ptr;
        if (!rw) {
            rw = (pthread_rwlock_t *)malloc(sizeof(*rw));
            pthread_rwlock_init(rw, NULL);
            __atomic_store_n((pthread_rwlock_t **)&lock->Ptr, rw, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&init_lock);
    }
    return rw;
}

VOID InitializeSRWLock(PSRWLOCK lock)
{
    lock->Ptr = NULL;
    srw_lazy_init(lock);
}

VOID AcquireSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_rdlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockShared(PSRWLOCK lock)     { pthread_rwlock_unlock(srw_lazy_init(lock)); }
VOID AcquireSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_wrlock(srw_lazy_init(lock)); }
VOID ReleaseSRWLockExclusive(PSRWLOCK lock)  { pthread_rwlock_unlock(srw_lazy_init(lock)); }

/* ===================================================================== */
/* One-time initialisation                                               */
/* ===================================================================== */

/* Win32 semantics: the callback runs at most once for a given INIT_ONCE, and
 * a callback returning FALSE leaves it un-run so a later call retries. Ptr
 * doubles as the "done" flag. One global mutex covers every INIT_ONCE --
 * initialisation is rare, and the fast path never touches it. */
BOOL InitOnceExecuteOnce(PINIT_ONCE once, PINIT_ONCE_FN fn, PVOID param, PVOID *context)
{
    static pthread_mutex_t once_lock = PTHREAD_MUTEX_INITIALIZER;

    if (__atomic_load_n(&once->Ptr, __ATOMIC_ACQUIRE))
        return TRUE;

    pthread_mutex_lock(&once_lock);
    BOOL ok = TRUE;
    if (!once->Ptr) {
        ok = fn(once, param, context);
        if (ok)
            __atomic_store_n(&once->Ptr, (PVOID)1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&once_lock);
    return ok;
}

/* ===================================================================== */
/* Condition variables (paired with a CRITICAL_SECTION)                  */
/* ===================================================================== */

/* Forward decl; the definition lives further down with the wait helpers. */
static void deadline_from_ms(DWORD ms, struct timespec *ts);

static void cv_lazy_init(PCONDITION_VARIABLE cv)
{
    if (!cv->Ptr) {
        pthread_cond_t *c = (pthread_cond_t *)malloc(sizeof(pthread_cond_t));
        pthread_cond_init(c, NULL);
        /* Race window OK for typical Win32 usage (the CS is held). */
        cv->Ptr = c;
    }
}

VOID InitializeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv->Ptr = NULL;
    cv_lazy_init(cv);
}

BOOL SleepConditionVariableCS(PCONDITION_VARIABLE cv, PCRITICAL_SECTION cs, DWORD ms)
{
    cv_lazy_init(cv);
    if (!cs->LockSemaphore) InitializeCriticalSection(cs);
    pthread_cond_t  *c = (pthread_cond_t  *)cv->Ptr;
    pthread_mutex_t *m = (pthread_mutex_t *)cs->LockSemaphore;
    int blocked = gc_block();
    if (ms == INFINITE) {
        pthread_cond_wait(c, m);
        gc_unblock(blocked);
        return TRUE;
    }
    struct timespec ts;
    deadline_from_ms(ms, &ts);
    int rc = pthread_cond_timedwait(c, m, &ts);
    gc_unblock(blocked);
    if (rc == ETIMEDOUT) { SetLastError(WAIT_TIMEOUT); return FALSE; }
    return TRUE;
}

VOID WakeConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_signal((pthread_cond_t *)cv->Ptr);
}

VOID WakeAllConditionVariable(PCONDITION_VARIABLE cv)
{
    cv_lazy_init(cv);
    pthread_cond_broadcast((pthread_cond_t *)cv->Ptr);
}

/* ===================================================================== */
/* Waitable kernel objects                                               */
/* ===================================================================== */

typedef enum { K_EVENT, K_SEM, K_MUTEX, K_THREAD, K_TIMER, K_HEAP,
               K_FILEMAP, K_FILE, K_WAITABLE_TIMER } w32_kind;

#define W32_MAX_APC 16

typedef struct w32_object {
    w32_kind        kind;
    LONG            refcount;
    pthread_mutex_t lock;
    pthread_cond_t  cond;

    /* event */
    int             signaled;
    int             manual_reset;

    /* semaphore */
    long            sem_count;
    long            sem_max;

    /* mutex */
    DWORD           mtx_owner;
    int             mtx_recursion;

    /* thread */
    pthread_t       thread;
    int             thread_joinable;
    DWORD           tid;
    int             exited;
    DWORD           exit_code;
    int             suspend_count;
    pthread_cond_t  gate;
    LPTHREAD_START_ROUTINE start;
    LPVOID          start_param;
    int             priority;
    void           *gc;             /* its guest-CPU turn state, once it has joined */
    PAPCFUNC        apc_func[W32_MAX_APC];
    ULONG_PTR       apc_data[W32_MAX_APC];
    int             apc_count;

    /* timer-queue timer */
    int             timer_cancel;
    DWORD           timer_due;
    DWORD           timer_period;
    WAITORTIMERCALLBACK timer_cb;
    PVOID           timer_param;

    /* waitable timer */
    int             waitable_manual_reset;
    struct timespec waitable_due_time;
    int             waitable_triggered;
    int             waitable_armed;

    /* file mapping / fd-backed file handle */
    int             fd;
    SIZE_T          map_size;
    char           *file_path;
} w32_object;

/* pseudo handles for "current thread"/"current process" */
#define PSEUDO_CURRENT_PROCESS ((HANDLE)(LONG_PTR)-1)
#define PSEUDO_CURRENT_THREAD  ((HANDLE)(LONG_PTR)-2)

static __thread w32_object *t_self_obj = NULL;

/* A title usually sets a thread's priority from the thread that created it, so the
 * thread's turn state is reached through its object as well as from the thread itself. */
static void gc_bind_self(gc_thread *t)
{
    w32_object *o = t_self_obj;
    if (!o || o->kind != K_THREAD)
        return;
    o->gc = t;
    t->obj = o;
    t->urgent = o->priority >= THREAD_PRIORITY_TIME_CRITICAL;
}

static void gc_unbind(gc_thread *t)
{
    w32_object *o = t->obj;
    if (o && o->gc == t)
        o->gc = NULL;
}
static __thread DWORD       t_tid      = 0;
static volatile LONG        s_next_tid = 1000;

DWORD GetCurrentThreadId(void)
{
    if (t_tid == 0)
        t_tid = (DWORD)InterlockedIncrement(&s_next_tid);
    return t_tid;
}

DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }
HANDLE GetCurrentThread(void)   { return t_self_obj ? (HANDLE)t_self_obj : PSEUDO_CURRENT_THREAD; }
HANDLE GetCurrentProcess(void)  { return PSEUDO_CURRENT_PROCESS; }

static w32_object *obj_alloc(w32_kind kind)
{
    w32_object *o = (w32_object *)calloc(1, sizeof(w32_object));
    o->kind     = kind;
    o->refcount = 1;
    pthread_mutex_init(&o->lock, NULL);
    pthread_cond_init(&o->cond, NULL);
    pthread_cond_init(&o->gate, NULL);
    return o;
}

static void obj_release(w32_object *o)
{
    if (InterlockedDecrement(&o->refcount) > 0)
        return;
    if (o->kind == K_FILE) {
        if (o->fd >= 0) close(o->fd);
        free(o->file_path);
    } else if (o->kind == K_FILEMAP) {
        if (o->fd >= 0) close(o->fd);
    } else if (o->kind == K_WAITABLE_TIMER) {
        /* Waitable timers: no special cleanup needed */
    }
    pthread_mutex_destroy(&o->lock);
    pthread_cond_destroy(&o->cond);
    pthread_cond_destroy(&o->gate);
    free(o);
}

/* ---- fd-backed file handle (for the file-I/O HLE) -------------------- */
HANDLE w32_open_handle(int fd, const char *host_path)
{
    w32_object *o = obj_alloc(K_FILE);
    o->fd        = fd;
    o->file_path = host_path ? strdup(host_path) : NULL;
    return (HANDLE)o;
}

int w32_handle_fd(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    return (o && o->kind == K_FILE) ? o->fd : -1;
}

const char *w32_handle_path(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    return (o && o->kind == K_FILE) ? o->file_path : NULL;
}

BOOL CloseHandle(HANDLE h)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS ||
        h == INVALID_HANDLE_VALUE)
        return TRUE;
    obj_release((w32_object *)h);
    return TRUE;
}

BOOL DuplicateHandle(HANDLE srcProc, HANDLE src, HANDLE dstProc, PHANDLE dst,
                     DWORD access, BOOL inherit, DWORD options)
{
    (void)srcProc; (void)dstProc; (void)access; (void)inherit;
    if (!dst) return FALSE;
    if (src == PSEUDO_CURRENT_THREAD)  src = GetCurrentThread();
    if (src == PSEUDO_CURRENT_PROCESS) { *dst = src; return TRUE; }
    if (src == PSEUDO_CURRENT_THREAD || !src) { *dst = src; return TRUE; }
    w32_object *o = (w32_object *)src;
    InterlockedIncrement(&o->refcount);
    *dst = src;
    if (options & DUPLICATE_CLOSE_SOURCE)
        obj_release(o);
    return TRUE;
}

/* ---- deadline helper -------------------------------------------------- */
static void deadline_from_ms(DWORD ms, struct timespec *ts)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += ms / 1000;
    ts->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) { ts->tv_sec++; ts->tv_nsec -= 1000000000L; }
}

/* Run any pending user-APCs for the calling thread. Returns count run. */
static int drain_apcs(void)
{
    w32_object *o = t_self_obj;
    int run = 0;
    if (!o) return 0;
    pthread_mutex_lock(&o->lock);
    while (o->apc_count > 0) {
        PAPCFUNC  f = o->apc_func[0];
        ULONG_PTR d = o->apc_data[0];
        memmove(o->apc_func, o->apc_func + 1, sizeof(PAPCFUNC) * (o->apc_count - 1));
        memmove(o->apc_data, o->apc_data + 1, sizeof(ULONG_PTR) * (o->apc_count - 1));
        o->apc_count--;
        pthread_mutex_unlock(&o->lock);
        f(d);
        run++;
        pthread_mutex_lock(&o->lock);
    }
    pthread_mutex_unlock(&o->lock);
    return run;
}

static int timespec_before(const struct timespec *a, const struct timespec *b)
{
    return a->tv_sec != b->tv_sec ? a->tv_sec < b->tv_sec : a->tv_nsec < b->tv_nsec;
}

/* Signalled once the due time passes; latched, so a manual-reset timer stays
 * signalled until it is set or cancelled again. Caller holds o->lock. */
static int waitable_due(w32_object *o)
{
    struct timespec now;
    if (o->waitable_triggered) return 1;
    if (!o->waitable_armed) return 0;
    clock_gettime(CLOCK_REALTIME, &now);
    if (timespec_before(&now, &o->waitable_due_time)) return 0;
    o->waitable_triggered = 1;
    return 1;
}

/*
 * Wait on a single object. The object lock must NOT be held.
 * Returns WAIT_OBJECT_0 / WAIT_TIMEOUT.
 */
static DWORD wait_single(w32_object *o, DWORD ms)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    pthread_mutex_lock(&o->lock);
    DWORD result = WAIT_OBJECT_0;
    int blocked = 0;

    for (;;) {
        int ready = 0;
        switch (o->kind) {
        case K_EVENT:  ready = o->signaled; break;
        case K_THREAD: ready = o->exited;   break;
        case K_SEM:    ready = (o->sem_count > 0); break;
        case K_MUTEX:
            ready = (o->mtx_owner == 0 || o->mtx_owner == GetCurrentThreadId());
            break;
        case K_WAITABLE_TIMER: ready = waitable_due(o); break;
        default:       ready = 1; break;
        }
        if (ready) break;
        if (timed && ms == 0) { result = WAIT_TIMEOUT; break; }
        if (!blocked) blocked = gc_block();

        /* An armed timer has its own deadline. Waiting on the caller's alone
         * would sleep straight past the due time, so take whichever comes
         * first and re-test. */
        struct timespec until = ts;
        int bounded = timed;
        if (o->kind == K_WAITABLE_TIMER && o->waitable_armed &&
            (!timed || timespec_before(&o->waitable_due_time, &ts))) {
            until = o->waitable_due_time;
            bounded = 1;
        }
        int rc = bounded ? pthread_cond_timedwait(&o->cond, &o->lock, &until)
                         : pthread_cond_wait(&o->cond, &o->lock);
        if (rc == ETIMEDOUT && timed && !timespec_before(&until, &ts)) {
            result = WAIT_TIMEOUT; break;
        }
    }

    if (result == WAIT_OBJECT_0) {
        switch (o->kind) {
        case K_EVENT: if (!o->manual_reset) o->signaled = 0; break;
        case K_WAITABLE_TIMER:
            if (!o->waitable_manual_reset) { o->waitable_triggered = 0; o->waitable_armed = 0; }
            break;
        case K_SEM:   o->sem_count--; break;
        case K_MUTEX: o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion++; break;
        default: break;
        }
    }
    pthread_mutex_unlock(&o->lock);
    /* Taken back with the object's lock dropped: the thread that holds the
     * guest CPU may be about to signal this very object. */
    gc_unblock(blocked);
    return result;
}

DWORD WaitForSingleObject(HANDLE h, DWORD ms)
{
    if (!h || h == PSEUDO_CURRENT_THREAD || h == PSEUDO_CURRENT_PROCESS)
        return WAIT_OBJECT_0;
    return wait_single((w32_object *)h, ms);
}

DWORD WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return WaitForSingleObject(h, ms);
}

/*
 * WaitForMultipleObjects: polling implementation. Adequate for the light
 * multi-object waits the Xbox kernel HLE issues; not a high-throughput path.
 */
DWORD WaitForMultipleObjects(DWORD count, const HANDLE *handles, BOOL waitAll, DWORD ms)
{
    return WaitForMultipleObjectsEx(count, handles, waitAll, ms, FALSE);
}

DWORD WaitForMultipleObjectsEx(DWORD count, const HANDLE *handles, BOOL waitAll,
                               DWORD ms, BOOL alertable)
{
    struct timespec ts;
    int timed = (ms != INFINITE);
    if (timed) deadline_from_ms(ms, &ts);

    for (;;) {
        if (alertable && drain_apcs() > 0)
            return WAIT_IO_COMPLETION;

        if (waitAll) {
            DWORD got = 0;
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0) got++;
            if (got == count) return WAIT_OBJECT_0;
        } else {
            for (DWORD i = 0; i < count; i++)
                if (WaitForSingleObject(handles[i], 0) == WAIT_OBJECT_0)
                    return WAIT_OBJECT_0 + i;
        }

        if (timed) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > ts.tv_sec ||
                (now.tv_sec == ts.tv_sec && now.tv_nsec >= ts.tv_nsec))
                return WAIT_TIMEOUT;
        }
        {
            int blocked = gc_block();
            usleep(1000);
            gc_unblock(blocked);
        }
    }
}

/* ===================================================================== */
/* Events                                                                */
/* ===================================================================== */

HANDLE CreateEventA(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_EVENT);
    o->manual_reset = manualReset ? 1 : 0;
    o->signaled     = initialState ? 1 : 0;
    return (HANDLE)o;
}
HANDLE CreateEventW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, BOOL initialState, LPCWSTR name)
{
    (void)name;
    return CreateEventA(sa, manualReset, initialState, NULL);
}

BOOL SetEvent(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL ResetEvent(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL PulseEvent(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_EVENT) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->signaled = 1;
    pthread_cond_broadcast(&o->cond);
    o->signaled = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Semaphores                                                            */
/* ===================================================================== */

HANDLE CreateSemaphoreA(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_SEM);
    o->sem_count = initial;
    o->sem_max   = maximum;
    return (HANDLE)o;
}
HANDLE CreateSemaphoreW(LPSECURITY_ATTRIBUTES sa, LONG initial, LONG maximum, LPCWSTR name)
{
    (void)name;
    return CreateSemaphoreA(sa, initial, maximum, NULL);
}

BOOL ReleaseSemaphore(HANDLE h, LONG releaseCount, PLONG previousCount)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_SEM) return FALSE;
    pthread_mutex_lock(&o->lock);
    if (previousCount) *previousCount = (LONG)o->sem_count;
    o->sem_count += releaseCount;
    if (o->sem_max && o->sem_count > o->sem_max) o->sem_count = o->sem_max;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Mutexes                                                               */
/* ===================================================================== */

HANDLE CreateMutexA(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCSTR name)
{
    (void)sa; (void)name;
    w32_object *o = obj_alloc(K_MUTEX);
    if (initialOwner) { o->mtx_owner = GetCurrentThreadId(); o->mtx_recursion = 1; }
    return (HANDLE)o;
}
HANDLE CreateMutexW(LPSECURITY_ATTRIBUTES sa, BOOL initialOwner, LPCWSTR name)
{
    (void)name;
    return CreateMutexA(sa, initialOwner, NULL);
}

BOOL ReleaseMutex(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_MUTEX) {
        SetLastError(ERROR_INVALID_HANDLE);
        return FALSE;
    }
    pthread_mutex_lock(&o->lock);
    if (o->mtx_owner != GetCurrentThreadId()) {
        pthread_mutex_unlock(&o->lock);
        SetLastError(ERROR_NOT_OWNER);
        return FALSE;
    }
    if (--o->mtx_recursion <= 0) {
        o->mtx_owner = 0;
        o->mtx_recursion = 0;
        pthread_cond_broadcast(&o->cond);
    }
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Threads                                                               */
/* ===================================================================== */

static void *thread_trampoline(void *arg)
{
    w32_object *o = (w32_object *)arg;
    t_self_obj = o;
    t_tid      = o->tid;

    /* CREATE_SUSPENDED gate */
    pthread_mutex_lock(&o->lock);
    while (o->suspend_count > 0)
        pthread_cond_wait(&o->gate, &o->lock);
    pthread_mutex_unlock(&o->lock);

    DWORD rc = o->start ? o->start(o->start_param) : 0;
    gc_block();   /* a thread that ran guest code is leaving the guest CPU */

    pthread_mutex_lock(&o->lock);
    o->exit_code = rc;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);

    obj_release(o);   /* drop the trampoline's reference */
    return NULL;
}

HANDLE CreateThread(LPSECURITY_ATTRIBUTES sa, SIZE_T stackSize,
                    LPTHREAD_START_ROUTINE start, LPVOID param,
                    DWORD flags, LPDWORD threadId)
{
    (void)sa;
    w32_object *o = obj_alloc(K_THREAD);
    o->start         = start;
    o->start_param   = param;
    o->tid           = (DWORD)InterlockedIncrement(&s_next_tid);
    o->suspend_count = (flags & CREATE_SUSPENDED) ? 1 : 0;
    o->refcount      = 2;   /* one for caller, one for the trampoline */

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stackSize)
        pthread_attr_setstacksize(&attr, stackSize < 65536 ? 65536 : stackSize);

    if (pthread_create(&o->thread, &attr, thread_trampoline, o) != 0) {
        pthread_attr_destroy(&attr);
        o->refcount = 1;
        obj_release(o);
        SetLastError(8 /* ERROR_NOT_ENOUGH_MEMORY */);
        return NULL;
    }
    pthread_attr_destroy(&attr);
    o->thread_joinable = 1;

    if (threadId) *threadId = o->tid;
    return (HANDLE)o;
}

VOID ExitThread(DWORD exitCode)
{
    w32_object *o = t_self_obj;
    gc_block();
    if (o) {
        pthread_mutex_lock(&o->lock);
        o->exit_code = exitCode;
        o->exited    = 1;
        o->signaled  = 1;
        pthread_cond_broadcast(&o->cond);
        pthread_mutex_unlock(&o->lock);
        obj_release(o);
    }
    pthread_exit(NULL);
}

BOOL GetExitCodeThread(HANDLE h, LPDWORD exitCode)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD || !exitCode) return FALSE;
    pthread_mutex_lock(&o->lock);
    *exitCode = o->exited ? o->exit_code : STILL_ACTIVE;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

DWORD ResumeThread(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    if (o->suspend_count > 0 && --o->suspend_count == 0)
        pthread_cond_broadcast(&o->gate);
    pthread_mutex_unlock(&o->lock);
    return prev;
}

DWORD SuspendThread(HANDLE h)
{
    /* True mid-run suspension is not supported on POSIX; only the
     * CREATE_SUSPENDED start gate is. Track the count for ResumeThread. */
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&o->lock);
    DWORD prev = (DWORD)o->suspend_count;
    o->suspend_count++;
    pthread_mutex_unlock(&o->lock);
    return prev;
}

BOOL TerminateThread(HANDLE h, DWORD exitCode)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_THREAD) return FALSE;
#if defined(__ANDROID__)
    /* Bionic deliberately does not implement pthread cancellation.  Keep
     * the Win32 handle state coherent; Android thread shutdown must be
     * cooperative at the emulated-kernel layer. */
#else
    pthread_cancel(o->thread);
#endif
    pthread_mutex_lock(&o->lock);
    o->exit_code = exitCode;
    o->exited    = 1;
    o->signaled  = 1;
    pthread_cond_broadcast(&o->cond);
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

BOOL SetThreadPriority(HANDLE h, int priority)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)h;
    if (o && o->kind == K_THREAD) {
        o->priority = priority;
        if (o->gc)
            ((gc_thread *)o->gc)->urgent = priority >= THREAD_PRIORITY_TIME_CRITICAL;
    }
    if (t_gc && (h == PSEUDO_CURRENT_THREAD || (o && o == t_self_obj)))
        t_gc->urgent = priority >= THREAD_PRIORITY_TIME_CRITICAL;
    return TRUE;   /* real RT priorities need privileges; tracked only */
}

int GetThreadPriority(HANDLE h)
{
    w32_object *o = (h == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)h;
    return (o && o->kind == K_THREAD) ? o->priority : THREAD_PRIORITY_NORMAL;
}

/* A yield that lets another thread run without falling behind every thread on the host.
 *
 * Darwin's sched_yield() is swtch_pri(0): it depresses the caller to priority 0 until it
 * next runs. A thread that yields in a loop -- Direct3D's fence hold waiting for the
 * push-buffer executor, the executor between passes, the APU frame thread -- then waits
 * behind every runnable thread on the machine, whatever its QoS, and on a busy host (an
 * 8-core Mac with a browser and the system's daemons) both halves of a frame starved
 * together: a Story scene's frame lasted 100-300 ms while the game and the executor ran
 * for 3 and 8 ms of a 54 ms probe window. thread_switch() with no option yields the CPU
 * to a thread that is ready at the caller's priority and keeps that priority.
 * RECOMP_YIELD_DEPRESS=1 restores sched_yield(). Elsewhere sched_yield() does not depress. */
static void host_yield(void)
{
#if defined(__APPLE__)
    static int depress = -1;
    if (depress < 0) {
        const char *e = getenv("RECOMP_YIELD_DEPRESS");
        depress = e && *e == '1';
    }
    if (!depress) {
        thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);
        return;
    }
#endif
    sched_yield();
}

VOID SwitchToThread(void) { gc_yield(); host_yield(); }

DWORD QueueUserAPC(PAPCFUNC func, HANDLE thread, ULONG_PTR data)
{
    w32_object *o = (thread == PSEUDO_CURRENT_THREAD) ? t_self_obj : (w32_object *)thread;
    if (!o || o->kind != K_THREAD) return 0;
    pthread_mutex_lock(&o->lock);
    DWORD ok = 0;
    if (o->apc_count < W32_MAX_APC) {
        o->apc_func[o->apc_count] = func;
        o->apc_data[o->apc_count] = data;
        o->apc_count++;
        ok = 1;
    }
    pthread_mutex_unlock(&o->lock);
    return ok;
}

/* ===================================================================== */
/* Sleep                                                                 */
/* ===================================================================== */

/* RECOMP_PRECISE_SLEEP (default 1): sleeps of a time-critical guest thread end on time,
 * and the thread asks the host for interactive service (Darwin: user-interactive QoS, which
 * also caps timer lateness at 1 ms). 0 sleeps as every other thread does. */
static int precise_sleep_wanted(void)
{
    static int on = -1;
    static __thread int qos_done;
    w32_object *o = t_self_obj;
    if (on < 0) {
        const char *e = getenv("RECOMP_PRECISE_SLEEP");
        on = !(e && *e == '0');
    }
    if (!on || !o || o->kind != K_THREAD || o->priority < THREAD_PRIORITY_TIME_CRITICAL)
        return 0;
#if defined(__APPLE__)
    if (!qos_done) {
        qos_done = 1;
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
#else
    (void)qos_done;
#endif
    return 1;
}

VOID Sleep(DWORD ms)
{
    if (ms == 0) { gc_yield(); host_yield(); return; }
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    int blocked = gc_block();
    if (precise_sleep_wanted()) {
        /* A time-critical thread wakes when it asked to. Darwin lets an ordinary thread's
         * timer fire late by up to a quarter of the interval (5 ms at most): a 15 ms sleep
         * of Def Jam's audio writer came back 3-4 ms late, past the end of the 50 ms ring it
         * refills, and the voice processor replayed the previous lap -- the crackle. Sleep in
         * steps of three quarters of what is left, so the lateness of each step is covered
         * by the rest, and yield through the last 0.2 ms. */
        struct timespec a;
        int64_t end;
        clock_gettime(CLOCK_MONOTONIC, &a);
        end = (int64_t)a.tv_sec * 1000000000LL + a.tv_nsec + (int64_t)ms * 1000000LL;
        for (;;) {
            struct timespec now, step;
            int64_t left;
            clock_gettime(CLOCK_MONOTONIC, &now);
            left = end - ((int64_t)now.tv_sec * 1000000000LL + now.tv_nsec);
            if (left <= 0)
                break;
            if (left <= 200000) {
                host_yield();
                continue;
            }
            left = left > 800000 ? left * 3 / 4 : left - 200000;
            step.tv_sec = (time_t)(left / 1000000000LL);
            step.tv_nsec = (long)(left % 1000000000LL);
            while (nanosleep(&step, &step) == -1 && errno == EINTR) { }
        }
    } else {
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    }
    gc_unblock(blocked);
}

DWORD SleepEx(DWORD ms, BOOL alertable)
{
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    Sleep(ms);
    if (alertable && drain_apcs() > 0)
        return WAIT_IO_COMPLETION;
    return 0;
}

/* ===================================================================== */
/* Timer queues (one helper thread per timer)                            */
/* ===================================================================== */

static void *timer_thread(void *arg)
{
    w32_object *o = (w32_object *)arg;
    int once = (o->timer_period == 0);

    /* initial due time */
    if (o->timer_due) Sleep(o->timer_due);
    if (!o->timer_cancel && o->timer_cb)
        o->timer_cb(o->timer_param, TRUE);

    while (!once && !o->timer_cancel) {
        Sleep(o->timer_period);
        if (o->timer_cancel) break;
        if (o->timer_cb) o->timer_cb(o->timer_param, TRUE);
    }
    obj_release(o);
    return NULL;
}

HANDLE CreateTimerQueue(void)
{
    /* A timer queue is just a grouping token here. */
    return (HANDLE)obj_alloc(K_TIMER);
}

BOOL DeleteTimerQueue(HANDLE timerQueue)
{
    return CloseHandle(timerQueue);
}

BOOL CreateTimerQueueTimer(PHANDLE newTimer, HANDLE timerQueue,
                           WAITORTIMERCALLBACK callback, PVOID param,
                           DWORD dueTime, DWORD period, ULONG flags)
{
    (void)timerQueue;
    if (flags & WT_EXECUTEONLYONCE) period = 0;
    w32_object *o = obj_alloc(K_TIMER);
    o->timer_cb     = callback;
    o->timer_param  = param;
    o->timer_due    = dueTime;
    o->timer_period = period;
    o->refcount     = 2;   /* caller + timer thread */

    if (pthread_create(&o->thread, NULL, timer_thread, o) != 0) {
        o->refcount = 1;
        obj_release(o);
        return FALSE;
    }
    pthread_detach(o->thread);
    if (newTimer) *newTimer = (HANDLE)o;
    return TRUE;
}

BOOL ChangeTimerQueueTimer(HANDLE timerQueue, HANDLE timer, ULONG dueTime, ULONG period)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_due    = dueTime;
    o->timer_period = period;
    return TRUE;
}

BOOL DeleteTimerQueueTimer(HANDLE timerQueue, HANDLE timer, HANDLE completionEvent)
{
    (void)timerQueue;
    w32_object *o = (w32_object *)timer;
    if (!o || o->kind != K_TIMER) return FALSE;
    o->timer_cancel = 1;
    if (completionEvent) SetEvent(completionEvent);
    obj_release(o);
    return TRUE;
}

struct w32_tp_args { PTP_SIMPLE_CALLBACK cb; PVOID ctx; };

static void *w32_tp_trampoline(void *arg)
{
    struct w32_tp_args *a = (struct w32_tp_args *)arg;
    a->cb(NULL, a->ctx);
    free(a);
    return NULL;
}

BOOL TrySubmitThreadpoolCallback(PTP_SIMPLE_CALLBACK callback,
                                 PVOID context, PVOID env)
{
    (void)env;
    /* Run on a throwaway detached thread. */
    struct w32_tp_args *a = (struct w32_tp_args *)malloc(sizeof(*a));
    a->cb = callback; a->ctx = context;
    pthread_t th;
    if (pthread_create(&th, NULL, w32_tp_trampoline, a) != 0) { free(a); return FALSE; }
    pthread_detach(th);
    return TRUE;
}

/* ===================================================================== */
/* Waitable timers                                                       */
/* ===================================================================== */

HANDLE CreateWaitableTimerW(LPSECURITY_ATTRIBUTES sa, BOOL manualReset, LPCWSTR name)
{
    (void)sa;
    (void)name;
    w32_object *o = obj_alloc(K_WAITABLE_TIMER);
    o->waitable_manual_reset = manualReset;
    return (HANDLE)o;
}

BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *dueTime, LONG period,
                      PTIMERAPCROUTINE completion, PVOID arg, BOOL resume)
{
    w32_object *o = (w32_object *)h;
    (void)completion; (void)arg; (void)resume;
    if (!o || o->kind != K_WAITABLE_TIMER || !dueTime) return FALSE;
    pthread_mutex_lock(&o->lock);
    /* Win32 100ns units: negative is relative to now, positive is an absolute
     * FILETIME. ponytail: period is ignored -- one-shot only, revisit if a
     * title actually arms a repeating timer. */
    if (dueTime->QuadPart <= 0) {
        clock_gettime(CLOCK_REALTIME, &o->waitable_due_time);
        LONGLONG ns = -dueTime->QuadPart * 100LL;
        o->waitable_due_time.tv_sec  += (time_t)(ns / 1000000000LL);
        o->waitable_due_time.tv_nsec += (long)(ns % 1000000000LL);
        if (o->waitable_due_time.tv_nsec >= 1000000000L) {
            o->waitable_due_time.tv_sec++;
            o->waitable_due_time.tv_nsec -= 1000000000L;
        }
    } else {
        /* FILETIME epoch is 1601-01-01; Unix is 1970-01-01. */
        LONGLONG unix100ns = dueTime->QuadPart - 116444736000000000LL;
        o->waitable_due_time.tv_sec  = (time_t)(unix100ns / 10000000LL);
        o->waitable_due_time.tv_nsec = (long)((unix100ns % 10000000LL) * 100LL);
    }
    (void)period;
    o->waitable_armed = 1;
    o->waitable_triggered = 0;
    pthread_mutex_unlock(&o->lock);
    pthread_cond_broadcast(&o->cond);
    return TRUE;
}

BOOL CancelWaitableTimer(HANDLE h)
{
    w32_object *o = (w32_object *)h;
    if (!o || o->kind != K_WAITABLE_TIMER) return FALSE;
    pthread_mutex_lock(&o->lock);
    o->waitable_triggered = 0;
    o->waitable_armed = 0;
    pthread_mutex_unlock(&o->lock);
    return TRUE;
}

/* ===================================================================== */
/* Heap (thin wrapper over malloc; the single process heap)              */
/* ===================================================================== */

static w32_object s_process_heap = { .kind = K_HEAP };

HANDLE GetProcessHeap(void)                       { return (HANDLE)&s_process_heap; }
HANDLE HeapCreate(DWORD o, SIZE_T i, SIZE_T m)    { (void)o;(void)i;(void)m; return (HANDLE)&s_process_heap; }
BOOL   HeapDestroy(HANDLE h)                      { (void)h; return TRUE; }

LPVOID HeapAlloc(HANDLE heap, DWORD flags, SIZE_T bytes)
{
    (void)heap;
    return (flags & HEAP_ZERO_MEMORY) ? calloc(1, bytes ? bytes : 1)
                                      : malloc(bytes ? bytes : 1);
}
LPVOID HeapReAlloc(HANDLE heap, DWORD flags, LPVOID mem, SIZE_T bytes)
{
    (void)heap; (void)flags;
    return realloc(mem, bytes ? bytes : 1);
}
BOOL HeapFree(HANDLE heap, DWORD flags, LPVOID mem)
{
    (void)heap; (void)flags;
    free(mem);
    return TRUE;
}
SIZE_T HeapSize(HANDLE heap, DWORD flags, LPCVOID mem)
{
    (void)heap; (void)flags; (void)mem;
    return 0;   /* glibc malloc_usable_size could be used; not needed yet */
}

/* ===================================================================== */
/* Virtual memory                                                        */
/* ===================================================================== */

static int prot_from_page(DWORD protect)
{
    switch (protect & 0xFF) {
    case PAGE_NOACCESS:          return PROT_NONE;
    case PAGE_READONLY:          return PROT_READ;
    case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE:           return PROT_EXEC;
    case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default:                     return PROT_READ | PROT_WRITE;
    }
}

/* Length registry, defined with the view helpers below. Win32 frees by address
 * alone -- UnmapViewOfFile takes no length and VirtualFree(MEM_RELEASE) is
 * documented to take size 0 -- so the length has to be recoverable here or
 * munmap cannot be called at all. */
void view_register(void *addr, size_t len);
size_t view_take(const void *addr);

#if defined(__APPLE__)
/* Darwin has no MAP_FIXED_NOREPLACE, and the two mmap options are both wrong
 * for VirtualAlloc: MAP_FIXED silently unmaps whatever already occupies the
 * range, and a bare address hint can be relocated by the kernel for reasons
 * other than the range being taken -- so "we got a different address" is only
 * an approximation of "it was occupied", and a racy one.
 *
 * mach_vm_map with VM_FLAGS_FIXED is the exact primitive: it maps at the
 * address given, and returns KERN_NO_SPACE rather than displacing an existing
 * mapping. That is what Win32 promises, and this layer exists to keep the Xbox
 * HLE above it honest -- a VirtualAlloc that quietly replaced a live mapping
 * would corrupt whatever held it, far from the call that did it.
 *
 * Memory from mach_vm_map is released by munmap like any other, because the
 * BSD and Mach halves of Darwin share one VM map, so VirtualFree is unchanged.
 */
static void *mach_map_fixed(void *address, size_t size, int prot)
{
    mach_vm_address_t addr = (mach_vm_address_t)(uintptr_t)address;
    mach_vm_size_t len = (size + vm_page_size - 1) & ~((mach_vm_size_t)vm_page_size - 1);
    vm_prot_t vmprot = VM_PROT_NONE;

    if (prot & PROT_READ)  vmprot |= VM_PROT_READ;
    if (prot & PROT_WRITE) vmprot |= VM_PROT_WRITE;
    if (prot & PROT_EXEC)  vmprot |= VM_PROT_EXECUTE;

    kern_return_t kr = mach_vm_map(
        mach_task_self(),
        &addr,
        len,
        0,
        VM_FLAGS_FIXED,
        MEMORY_OBJECT_NULL,
        0,
        FALSE,
        vmprot,
        VM_PROT_ALL,
        VM_INHERIT_DEFAULT
    );
    if (kr != KERN_SUCCESS) {
        return MAP_FAILED;
    }
    return (void *)(uintptr_t)addr;
}
#endif

/* Length registry, defined with the view helpers below. Win32 frees by address
 * alone -- UnmapViewOfFile takes no length and VirtualFree(MEM_RELEASE) is
 * documented to take size 0 -- so the length has to be recoverable here or
 * munmap cannot be called at all. */
void view_register(void *addr, size_t len);
size_t view_take(const void *addr);

/* ---- Reserved arena ----------------------------------------------------
 *
 * One PROT_NONE reservation the caller owns, whose pages fixed-address
 * requests may take over (win32_reserve_arena in win32_compat.h).
 *
 * Linux hands out mmap addresses top-down, so the memory just above an
 * OS-chosen base already belongs to libraries and earlier allocations. A
 * request at base + 0x80000000 then meets someone else's mapping, and
 * MAP_FIXED_NOREPLACE rightly refuses it. Reserving the whole guest window
 * up front settles that, but only if a fixed request can replace our own
 * placeholder -- which MAP_FIXED_NOREPLACE cannot tell from a stranger's
 * mapping. So the pages still held as placeholder are tracked here: a request
 * that lies wholly on them maps with MAP_FIXED, one that touches a live page
 * fails as Win32 would, and a release puts the placeholder back instead of
 * leaving a hole another allocator can take. Inert until registered. */
enum { ARENA_OUTSIDE, ARENA_FREE, ARENA_LIVE, ARENA_MIXED };

static pthread_mutex_t s_arena_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t s_arena_lo, s_arena_hi;
static size_t    s_arena_page;
static uint8_t  *s_arena_live;          /* bitmap, one bit per page */

BOOL win32_reserve_arena(void *base, size_t size)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t pages = size / page;
    uint8_t *bits = (uint8_t *)calloc((pages + 7) / 8, 1);

    if (!bits || ((uintptr_t)base | size) & (page - 1)) {
        free(bits);
        return FALSE;
    }
    pthread_mutex_lock(&s_arena_lock);
    free(s_arena_live);
    s_arena_live = bits;
    s_arena_page = page;
    s_arena_lo = (uintptr_t)base;
    s_arena_hi = (uintptr_t)base + size;
    pthread_mutex_unlock(&s_arena_lock);
    return TRUE;
}

/* Caller holds s_arena_lock. */
static int arena_state_locked(uintptr_t lo, size_t len)
{
    uintptr_t hi = lo + len;
    size_t first, last, i, live = 0;

    if (!s_arena_live || len == 0 || hi <= s_arena_lo || lo >= s_arena_hi)
        return ARENA_OUTSIDE;
    if (lo < s_arena_lo || hi > s_arena_hi)
        return ARENA_MIXED;             /* straddles the edge */
    first = (lo - s_arena_lo) / s_arena_page;
    last  = (hi - s_arena_lo + s_arena_page - 1) / s_arena_page;
    for (i = first; i < last; i++)
        live += (s_arena_live[i >> 3] >> (i & 7)) & 1;
    if (live == 0) return ARENA_FREE;
    return live == last - first ? ARENA_LIVE : ARENA_MIXED;
}

static int arena_state(const void *addr, size_t len)
{
    int s;
    pthread_mutex_lock(&s_arena_lock);
    s = arena_state_locked((uintptr_t)addr, len);
    pthread_mutex_unlock(&s_arena_lock);
    return s;
}

static void arena_mark(const void *addr, size_t len, int live)
{
    uintptr_t lo = (uintptr_t)addr;
    size_t first, last, i;

    pthread_mutex_lock(&s_arena_lock);
    if (arena_state_locked(lo, len) != ARENA_OUTSIDE && lo >= s_arena_lo
            && lo + len <= s_arena_hi) {
        first = (lo - s_arena_lo) / s_arena_page;
        last  = (lo + len - s_arena_lo + s_arena_page - 1) / s_arena_page;
        for (i = first; i < last; i++) {
            if (live) s_arena_live[i >> 3] |= (uint8_t)(1u << (i & 7));
            else      s_arena_live[i >> 3] &= (uint8_t)~(1u << (i & 7));
        }
    }
    pthread_mutex_unlock(&s_arena_lock);
}

/* Returns 1 if the range was inside the arena and is placeholder again,
 * 0 if it is not the arena's to take back. The whole arena released at once
 * is the owner giving it up: unmap it and forget it. */
static int arena_release(void *addr, size_t len, BOOL *ok)
{
    int s;
    pthread_mutex_lock(&s_arena_lock);
    s = arena_state_locked((uintptr_t)addr, len);
    if (s == ARENA_OUTSIDE) {
        pthread_mutex_unlock(&s_arena_lock);
        return 0;
    }
    if ((uintptr_t)addr == s_arena_lo && (uintptr_t)addr + len == s_arena_hi) {
        *ok = munmap(addr, len) == 0;
        free(s_arena_live);
        s_arena_live = NULL;
        s_arena_lo = s_arena_hi = 0;
        pthread_mutex_unlock(&s_arena_lock);
        return 1;
    }
    pthread_mutex_unlock(&s_arena_lock);
    if (s == ARENA_MIXED && ((uintptr_t)addr < s_arena_lo
                             || (uintptr_t)addr + len > s_arena_hi)) {
        *ok = FALSE;
        return 1;
    }
    *ok = mmap(addr, len, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED,
               -1, 0) != MAP_FAILED;
    if (*ok) arena_mark(addr, len, 0);
    return 1;
}

LPVOID VirtualAlloc(LPVOID address, SIZE_T size, DWORD allocationType, DWORD protect)
{
    int prot  = prot_from_page(protect);
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    int arena = address ? arena_state(address, size) : ARENA_OUTSIDE;

    /* MEM_COMMIT on a region already reserved by a prior VirtualAlloc:
     * just adjust protection. Arena placeholder is not such a region -- it
     * takes the fresh-mapping path below, as unmapped memory would. */
    if ((allocationType & MEM_COMMIT) && !(allocationType & MEM_RESERVE) && address
            && arena != ARENA_FREE && arena != ARENA_MIXED) {
        if (mprotect(address, size, prot) == 0)
            return address;
        /* fall through to a fresh mapping */
    }

    if (arena == ARENA_LIVE || arena == ARENA_MIXED) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }

#if defined(MAP_FIXED_NOREPLACE)
    if (address) flags |= MAP_FIXED_NOREPLACE;
#endif
    if (arena == ARENA_FREE)
        flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED;

    void *p;
#if defined(__APPLE__)
    if (address && arena != ARENA_FREE) {
        p = mach_map_fixed(address, size, prot ? prot : PROT_READ | PROT_WRITE);
    } else
#endif
    p = mmap(address, size, prot ? prot : PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
    if (arena == ARENA_FREE)
        arena_mark(p, size, 1);
    /* Remember the length: VirtualFree(MEM_RELEASE) is passed size 0 by every
     * Win32 caller, and munmap cannot be called without one. */
    view_register(p, size);
#if !defined(MAP_FIXED_NOREPLACE) && !defined(__APPLE__)
    /* Older kernels without MAP_FIXED_NOREPLACE: plain MAP_FIXED would silently
     * unmap whatever already lives there, so we pass the address as a hint and
     * treat a different result as "taken". Apple goes through mach_map_fixed
     * above, which reports that properly instead of inferring it. */
    if (address && p != address) {
        munmap(p, size);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }
#endif
    return p;
}

BOOL VirtualFree(LPVOID address, SIZE_T size, DWORD freeType)
{
    if (freeType & MEM_RELEASE) {
        /* Win32 MEM_RELEASE passes size 0 and frees the whole allocation, so
         * the length comes from the registry VirtualAlloc filled in. Returning
         * TRUE without unmapping -- as this used to -- made every release a
         * silent no-op: the caller believed the address was free, the next
         * allocation there failed, and nothing connected the two. */
        size_t len = view_take(address);
        BOOL ok;
        if (size == 0) size = len;
        if (size == 0) return FALSE;
        if (arena_release(address, size, &ok))
            return ok;
        return munmap(address, size) == 0;
    }
    if (freeType & MEM_DECOMMIT)
        return mprotect(address, size, PROT_NONE) == 0;
    return TRUE;
}

BOOL VirtualProtect(LPVOID address, SIZE_T size, DWORD newProtect, PDWORD oldProtect)
{
    uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);

    if (oldProtect) *oldProtect = PAGE_READWRITE;
    /* The console's devices are laid out in 4 KB pages and the callers close
     * them one at a time. Where the host page is larger, mprotect would close
     * the neighbours as well, so such a range goes to the trap layer, which
     * keeps the neighbours working as memory. */
    if (((uintptr_t)address | (uintptr_t)size) & (page - 1)) {
        if (newProtect == PAGE_NOACCESS)
            return mmio_trap_close(address, size) == 0;
        if (mmio_trap_remove(address) == 0)
            return TRUE;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;   /* a finer protection than the host has */
    }
    return mprotect(address, size, prot_from_page(newProtect)) == 0;
}

/* ===================================================================== */
/* Time                                                                  */
/* ===================================================================== */

/* 100-ns intervals between 1601-01-01 and 1970-01-01 */
#define FILETIME_EPOCH_DIFF 116444736000000000ULL

VOID GetSystemTimeAsFileTime(LPFILETIME ft)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ULONGLONG t = FILETIME_EPOCH_DIFF
                + (ULONGLONG)ts.tv_sec * 10000000ULL
                + (ULONGLONG)ts.tv_nsec / 100ULL;
    ft->dwLowDateTime  = (DWORD)(t & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(t >> 32);
}

static void fill_systemtime(LPSYSTEMTIME st, const struct tm *tm, long nsec)
{
    st->wYear         = (WORD)(tm->tm_year + 1900);
    st->wMonth        = (WORD)(tm->tm_mon + 1);
    st->wDayOfWeek    = (WORD)tm->tm_wday;
    st->wDay          = (WORD)tm->tm_mday;
    st->wHour         = (WORD)tm->tm_hour;
    st->wMinute       = (WORD)tm->tm_min;
    st->wSecond       = (WORD)tm->tm_sec;
    st->wMilliseconds = (WORD)(nsec / 1000000L);
}

VOID GetSystemTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    gmtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

VOID GetLocalTime(LPSYSTEMTIME st)
{
    struct timespec ts; struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    fill_systemtime(st, &tm, ts.tv_nsec);
}

ULONGLONG GetTickCount64(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)ts.tv_nsec / 1000000ULL;
}

DWORD GetTickCount(void) { return (DWORD)GetTickCount64(); }

BOOL QueryPerformanceCounter(PLARGE_INTEGER count)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (LONGLONG)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return TRUE;
}

BOOL QueryPerformanceFrequency(PLARGE_INTEGER freq)
{
    freq->QuadPart = 1000000000LL;   /* QPC is in nanoseconds */
    return TRUE;
}

/* ===================================================================== */
/* Misc                                                                  */
/* ===================================================================== */

VOID OutputDebugStringA(LPCSTR str)
{
    if (str) fputs(str, stderr);
}

VOID ExitProcess(UINT exitCode) { exit((int)exitCode); }

BOOL IsDebuggerPresent(void)
{
#if defined(__APPLE__)
    /* Darwin: KERN_PROC_PID reports P_TRACED when a debugger is attached. */
    struct kinfo_proc info;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    size_t size = sizeof(info);
    memset(&info, 0, size);
    if (sysctl(mib, sizeof(mib), &info, &size, NULL, 0) != 0) return FALSE;
    return (info.kp_proc.p_flag & P_TRACED) != 0;
#else
    /* Linux: a non-zero TracerPid in /proc/self/status means ptrace is attached. */
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return FALSE;
    char line[256];
    BOOL traced = FALSE;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            traced = strtol(line + 10, NULL, 10) != 0;
            break;
        }
    }
    fclose(f);
    return traced;
#endif
}

VOID DebugBreak(void) {
    /* Not __debugbreak(): that is an MSVC intrinsic, and this file is the
     * half that MSVC never compiles. SIGTRAP is the POSIX equivalent --
     * continuable under a debugger, fatal without one, as on Windows. */
    raise(SIGTRAP);
}

VOID SecureZeroMemory(PVOID ptr, SIZE_T cnt)
{
#if defined(__APPLE__)
    memset_s(ptr, cnt, 0, cnt);
#elif defined(__ANDROID__)
    /* A volatile byte loop has the same do-not-elide property and works on
     * every Android API level supported by this runtime. */
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (cnt--) *p++ = 0;
#else
    explicit_bzero(ptr, cnt);
#endif
}

unsigned int _clearfp(void)
{
    feclearexcept(FE_ALL_EXCEPT);
    return 0;
}

/* ===================================================================== */
/* Win32 file API on POSIX (open/read/write/fstat-backed)                */
/* ===================================================================== */

#include <fcntl.h>
#include <sys/stat.h>

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    (void)share; (void)sa; (void)flags; (void)templ;
    if (!name) { SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }

    int rw = O_RDONLY;
    int wantW = (access & (GENERIC_WRITE | GENERIC_ALL)) != 0;
    int wantR = (access & (GENERIC_READ  | GENERIC_ALL)) != 0;
    if (wantW && wantR) rw = O_RDWR;
    else if (wantW)     rw = O_WRONLY;

    int extra = 0;
    switch (disp) {
    case CREATE_NEW:        extra = O_CREAT | O_EXCL;  break;
    case CREATE_ALWAYS:     extra = O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING:     extra = 0;                 break;
    case OPEN_ALWAYS:       extra = O_CREAT;           break;
    case TRUNCATE_EXISTING: extra = O_TRUNC;           break;
    default:                extra = 0;                 break;
    }
    if ((extra & (O_CREAT | O_TRUNC)) && rw == O_RDONLY) rw = O_RDWR;

    /* Normalise embedded Windows-style backslashes before open(). */
    char norm[1024];
    snprintf(norm, sizeof(norm), "%s", name);
    xbox_path_normalize(norm);
    int fd = open(norm, rw | extra, 0644);
    if (fd < 0) { SetLastError(ERROR_FILE_NOT_FOUND); return INVALID_HANDLE_VALUE; }
    return w32_open_handle(fd, name);
}

HANDLE CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                   LPSECURITY_ATTRIBUTES sa, DWORD disp,
                   DWORD flags, HANDLE templ)
{
    /* Basic UTF-16 -> UTF-8 (ASCII path of WideCharToMultiByte). */
    char buf[1024];
    int len = WideCharToMultiByte(CP_UTF8, 0, name, -1, buf, sizeof(buf), NULL, NULL);
    if (len <= 0) buf[0] = '\0';
    return CreateFileA(buf, access, share, sa, disp, flags, templ);
}

BOOL ReadFile(HANDLE h, LPVOID buf, DWORD len, LPDWORD nread, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nread) *nread = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = read(fd, buf, len);
    if (n < 0)  { if (nread) *nread = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nread)  *nread = (DWORD)n;
    return TRUE;
}

BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD len, LPDWORD nwritten, void *overlapped)
{
    (void)overlapped;
    int fd = w32_handle_fd(h);
    if (fd < 0) { if (nwritten) *nwritten = 0; SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ssize_t n = write(fd, buf, len);
    if (n < 0)  { if (nwritten) *nwritten = 0; SetLastError(ERROR_GEN_FAILURE);    return FALSE; }
    if (nwritten) *nwritten = (DWORD)n;
    return TRUE;
}

DWORD GetFileSize(HANDLE h, LPDWORD high)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return INVALID_FILE_SIZE;
    struct stat st;
    if (fstat(fd, &st) != 0) return INVALID_FILE_SIZE;
    if (high) *high = (DWORD)(((uint64_t)st.st_size >> 32) & 0xFFFFFFFFu);
    return (DWORD)(st.st_size & 0xFFFFFFFFu);
}

BOOL GetFileSizeEx(HANDLE h, PLARGE_INTEGER size)
{
    int fd = w32_handle_fd(h);
    if (fd < 0 || !size) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    struct stat st;
    if (fstat(fd, &st) != 0) { SetLastError(ERROR_GEN_FAILURE); return FALSE; }
    size->QuadPart = (LONGLONG)st.st_size;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE h)
{
    int fd = w32_handle_fd(h);
    if (fd < 0) return FALSE;
    return fsync(fd) == 0;
}

/* ===================================================================== */
/* Keyboard + window helpers -- stubs. Real keyboard polling will come   */
/* via SDL_GetKeyboardState when main.c gets its SDL2 port.              */
/* ===================================================================== */

SHORT GetAsyncKeyState(int vKey)          { (void)vKey; return 0; }
HWND  FindWindowA(LPCSTR c, LPCSTR w)     { (void)c; (void)w; return NULL; }
HWND  GetActiveWindow(void)               { return NULL; }
BOOL  SetWindowTextA(HWND h, LPCSTR t)    { (void)h; (void)t; return TRUE; }
int   GetWindowTextA(HWND h, LPSTR t, int n) { (void)h; (void)t; (void)n; return 0; }
BOOL  EnumWindows(WNDENUMPROC p, LPARAM l) { (void)p; (void)l; return FALSE; }

int MessageBoxA(HWND h, LPCSTR text, LPCSTR caption, UINT type)
{
    (void)h; (void)type;
    fprintf(stderr, "[%s] %s\n", caption ? caption : "MessageBox",
                                   text    ? text    : "");
    return 1;   /* IDOK */
}

/* Message-loop stubs: no Win32 messages on POSIX (SDL events drive the
 * d3d8_gl backend; this layer is just for the game's Win32 message pump). */
BOOL    PeekMessageA(LPMSG m, HWND w, UINT a, UINT b, UINT f)
{ (void)m; (void)w; (void)a; (void)b; (void)f; return FALSE; }
BOOL    TranslateMessage(const MSG *m) { (void)m; return TRUE; }
LRESULT DispatchMessageA(const MSG *m) { (void)m; return 0; }

/* XInput stub: real gamepad is wired through input_compat (SDL2). */
DWORD XInputGetState(DWORD idx, XINPUT_STATE *state)
{ (void)idx; if (state) memset(state, 0, sizeof(*state)); return ERROR_DEVICE_NOT_CONNECTED; }

BOOL TerminateProcess(HANDLE process, UINT exitCode)
{
    (void)process;
    exit((int)exitCode);
}

VOID OutputDebugStringW(LPCWSTR str)
{
    if (!str) return;
    for (const WCHAR *p = str; *p; p++)
        fputc((*p < 128) ? (int)*p : '?', stderr);
}

/*
 * Minimal MultiByteToWideChar / WideCharToMultiByte. Handles UTF-8 and a
 * latin-1 interpretation of CP_ACP -- enough for path/name strings.
 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR mb, int mbCount,
                        LPWSTR wide, int wideCount)
{
    (void)flags;
    if (!mb) return 0;
    int srcLen = (mbCount < 0) ? (int)strlen(mb) + 1 : mbCount;
    int out = 0;

    for (int i = 0; i < srcLen; ) {
        unsigned int cpval;
        unsigned char c = (unsigned char)mb[i];

        if (cp == CP_UTF8 && c >= 0x80) {
            if ((c & 0xE0) == 0xC0 && i + 1 < srcLen) {
                cpval = ((c & 0x1F) << 6) | (mb[i+1] & 0x3F); i += 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < srcLen) {
                cpval = ((c & 0x0F) << 12) | ((mb[i+1] & 0x3F) << 6) |
                        (mb[i+2] & 0x3F); i += 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < srcLen) {
                cpval = ((c & 0x07) << 18) | ((mb[i+1] & 0x3F) << 12) |
                        ((mb[i+2] & 0x3F) << 6) | (mb[i+3] & 0x3F); i += 4;
            } else { cpval = c; i += 1; }
        } else {
            cpval = c; i += 1;   /* ASCII / latin-1 */
        }

        if (cpval > 0xFFFF) cpval = '?';   /* no surrogate pairs */
        if (wideCount > 0) {
            if (out >= wideCount) return 0;
            wide[out] = (WCHAR)cpval;
        }
        out++;
    }
    return out;
}

int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR wide, int wideCount,
                        LPSTR mb, int mbCount, LPCSTR defChar, PBOOL usedDef)
{
    (void)flags; (void)defChar; (void)usedDef;
    if (!wide) return 0;
    int srcLen = wideCount;
    if (srcLen < 0) { srcLen = 0; while (wide[srcLen]) srcLen++; srcLen++; }
    int out = 0;

    for (int i = 0; i < srcLen; i++) {
        unsigned int cpval = wide[i];
        char buf[4]; int n;
        if (cp == CP_UTF8 && cpval >= 0x80) {
            if (cpval < 0x800) {
                buf[0] = (char)(0xC0 | (cpval >> 6));
                buf[1] = (char)(0x80 | (cpval & 0x3F)); n = 2;
            } else {
                buf[0] = (char)(0xE0 | (cpval >> 12));
                buf[1] = (char)(0x80 | ((cpval >> 6) & 0x3F));
                buf[2] = (char)(0x80 | (cpval & 0x3F)); n = 3;
            }
        } else {
            buf[0] = (char)(cpval > 0xFF ? '?' : cpval); n = 1;
        }
        if (mbCount > 0) {
            if (out + n > mbCount) return 0;
            for (int k = 0; k < n; k++) mb[out + k] = buf[k];
        }
        out += n;
    }
    return out;
}

/* ===================================================================== */
/* File mapping (memfd-backed) -- true aliased mirror views              */
/* ===================================================================== */

/* Registry of active views: UnmapViewOfFile takes no length, so we must
 * recover the mapping length here for munmap. */
typedef struct { void *addr; size_t len; } w32_view;
static w32_view        s_views[512];
static pthread_mutex_t s_views_lock = PTHREAD_MUTEX_INITIALIZER;

void view_register(void *addr, size_t len)
{
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (!s_views[i].addr) { s_views[i].addr = addr; s_views[i].len = len; break; }
    pthread_mutex_unlock(&s_views_lock);
}

/* Non-destructive counterpart to view_take, and interior-aware: VirtualQuery
 * is asked about addresses *within* a region at least as often as about its
 * base -- a translated guest VA lands in the middle of the 64 MB window.
 * Picks the containing region and reports where it starts. */
int view_lookup(const void *addr, void **base_out, size_t *len_out)
{
    int found = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++) {
        if (!s_views[i].addr)
            continue;
        uintptr_t lo = (uintptr_t)s_views[i].addr;
        uintptr_t hi = lo + s_views[i].len;
        if ((uintptr_t)addr >= lo && (uintptr_t)addr < hi) {
            if (base_out) *base_out = s_views[i].addr;
            if (len_out)  *len_out  = s_views[i].len;
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&s_views_lock);
    return found;
}

size_t view_take(const void *addr)
{
    size_t len = 0;
    pthread_mutex_lock(&s_views_lock);
    for (int i = 0; i < 512; i++)
        if (s_views[i].addr == addr) { len = s_views[i].len; s_views[i].addr = NULL; break; }
    pthread_mutex_unlock(&s_views_lock);
    return len;
}

/* An unnamed file descriptor that ftruncate and mmap both accept. Linux has
 * memfd_create for this; elsewhere an immediately-unlinked temp file does. */
static int anon_map_fd(const char *name)
{
#if defined(__APPLE__)
    static volatile LONG map_counter = 0;
    char shm_name[32];
    LONG seq = InterlockedIncrement(&map_counter);
    const char *base = name ? name : "xbox_map";
    snprintf(shm_name, sizeof(shm_name), "/%s_%ld", base, seq);
    int fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd >= 0) shm_unlink(shm_name);
    return fd;
#elif defined(__ANDROID__)
    /* bionic exposes memfd_create only from API 30. The syscall exists on the
     * arm64 kernels used by API 29 devices, so keep minSdk 29 linkable. */
    return (int)syscall(__NR_memfd_create, name ? name : "xbox_map", 0);
#else
    return memfd_create(name ? name : "xbox_map", 0);
#endif
}

HANDLE CreateFileMappingA(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCSTR name)
{
    (void)file; (void)sa; (void)protect;
    SIZE_T size = ((SIZE_T)maxSizeHigh << 32) | maxSizeLow;
    if (size == 0) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }

    int fd = anon_map_fd(name);
    if (fd < 0) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return NULL;
    }

    w32_object *o = obj_alloc(K_FILEMAP);
    o->fd       = fd;
    o->map_size = size;
    return (HANDLE)o;
}

HANDLE CreateFileMappingW(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect,
                          DWORD maxSizeHigh, DWORD maxSizeLow, LPCWSTR name)
{
    (void)name;
    return CreateFileMappingA(file, sa, protect, maxSizeHigh, maxSizeLow, NULL);
}

LPVOID MapViewOfFileEx(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow,
                       SIZE_T count, LPVOID baseAddr)
{
    w32_object *o = (w32_object *)mapping;
    if (!o || o->kind != K_FILEMAP) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }

    off_t  off = ((off_t)offHigh << 32) | offLow;
    SIZE_T len = count ? count : (o->map_size - (SIZE_T)off);
    int prot   = PROT_READ | ((access != FILE_MAP_READ) ? PROT_WRITE : 0);
    int flags  = MAP_SHARED;
    int arena  = baseAddr ? arena_state(baseAddr, len) : ARENA_OUTSIDE;

    if (arena == ARENA_LIVE || arena == ARENA_MIXED) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }

    /* Win32 MapViewOfFileEx *fails* when the requested address is unavailable.
     * Plain MAP_FIXED does the opposite: it silently unmaps whatever is there
     * and succeeds. The Xbox memory model asks for 28 mirror views at computed
     * addresses, so with a base the OS chose rather than one we picked, that
     * difference is the process quietly destroying its own libraries and heap
     * and dying somewhere unrelated a moment later. */
    if (arena == ARENA_FREE) {
        /* Our own placeholder: replacing it is the point. */
        flags |= MAP_FIXED;
    } else if (baseAddr) {
#if defined(MAP_FIXED_NOREPLACE)
        flags |= MAP_FIXED_NOREPLACE;
#elif defined(__APPLE__)
        /* Darwin has no MAP_FIXED_NOREPLACE. Claim the range first with
         * mach_vm_map(VM_FLAGS_FIXED), which refuses rather than displaces;
         * MAP_FIXED below can then only replace the placeholder we now own. */
        if (mach_map_fixed(baseAddr, len, PROT_READ | PROT_WRITE) == MAP_FAILED) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return NULL;
        }
        flags |= MAP_FIXED;
#else
        flags |= MAP_FIXED;
#endif
    }

    void *p = mmap(baseAddr, len, prot, flags, o->fd, off);
    if (p == MAP_FAILED) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (baseAddr && p != baseAddr) {
        /* MAP_FIXED_NOREPLACE hands back a different address instead of
         * failing on some kernels; treat that as the refusal it means. */
        munmap(p, len);
        SetLastError(ERROR_INVALID_ADDRESS);
        return NULL;
    }
    if (arena == ARENA_FREE)
        arena_mark(p, len, 1);
    view_register(p, len);
    return p;
}

LPVOID MapViewOfFile(HANDLE mapping, DWORD access, DWORD offHigh, DWORD offLow, SIZE_T count)
{
    return MapViewOfFileEx(mapping, access, offHigh, offLow, count, NULL);
}

BOOL UnmapViewOfFile(LPCVOID baseAddr)
{
    size_t len = view_take(baseAddr);
    BOOL ok;
    if (len == 0) return FALSE;
    if (arena_release((void *)baseAddr, len, &ok))
        return ok;
    return munmap((void *)baseAddr, len) == 0;
}

/* ===================================================================== */
/* VirtualQuery                                                           */
/* ===================================================================== */

/*
 * Answers from the view registry rather than from a constant.
 *
 * This used to report RegionSize 0x1000, MEM_COMMIT, PAGE_READWRITE and
 * AllocationBase NULL for every address it was handed, mapped or not. Four
 * kernel entry points are built on it, and one of them chooses a deallocator
 * with it: MmFreeContiguousMemory frees via VirtualFree only when
 * AllocationBase equals the pointer, so a hardcoded NULL sent every
 * contiguous buffer -- all of which come from VirtualAlloc, i.e. mmap -- to
 * _aligned_free, which is free(). The allocator aborts on the foreign
 * pointer. MmQueryAllocationSize answered 0x1000 for everything and
 * NtQueryVirtualMemory called unmapped addresses committed and readable.
 *
 * Everything the shim maps -- VirtualAlloc and MapViewOfFileEx alike -- is in
 * the registry, so it can answer for exactly the memory it owns and say
 * MEM_FREE for the rest. Saying MEM_FREE for an address it did not map is the
 * honest answer: a host heap pointer is not a Win32 reservation, and the
 * callers that branch on this want to know which allocator owns the pointer.
 */
SIZE_T VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION buffer, SIZE_T length)
{
    if (!buffer || length < sizeof(*buffer)) return 0;
    memset(buffer, 0, sizeof(*buffer));

    void  *base = NULL;
    size_t len  = 0;

    if (!view_lookup(address, &base, &len)) {
        /* Not ours. Report it free rather than inventing a committed page. */
        buffer->BaseAddress    = (PVOID)address;
        buffer->AllocationBase = NULL;
        buffer->RegionSize     = 0;
        buffer->State          = MEM_FREE;
        buffer->Protect        = PAGE_NOACCESS;
        buffer->Type           = 0;
        return sizeof(*buffer);
    }

    buffer->BaseAddress      = (PVOID)address;
    buffer->AllocationBase   = base;
    buffer->AllocationProtect = PAGE_READWRITE;
    /* From the queried address to the end of the region, which is what Win32
     * reports and what callers sizing a copy out of it depend on. */
    buffer->RegionSize       = len - (size_t)((uintptr_t)address - (uintptr_t)base);
    buffer->State            = MEM_COMMIT;
    buffer->Protect          = PAGE_READWRITE;
    buffer->Type             = MEM_PRIVATE;
    return sizeof(*buffer);
}

BOOL GlobalMemoryStatusEx(LPMEMORYSTATUSEX b)
{
    if (!b) return FALSE;
#if defined(__APPLE__)
    /* Darwin has no sysinfo(2): physical memory comes from sysctl hw.memsize,
     * swap from vm.swapusage, the free page count from the Mach VM statistics. */
    uint64_t memsize = 0;
    size_t   len     = sizeof(memsize);
    int oid_memsize[] = { CTL_HW, HW_MEMSIZE };
    if (sysctl(oid_memsize, 2, &memsize, &len, NULL, 0) != 0) return FALSE;

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) page = 4096;

    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    ULONGLONG avail = 0;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          (host_info64_t)&vm, &count) == KERN_SUCCESS)
        avail = ((ULONGLONG)vm.free_count + vm.inactive_count) * (ULONGLONG)page;

    struct xsw_usage swap;
    len = sizeof(swap);
    int oid_swapusage[] = { CTL_VM, VM_SWAPUSAGE };
    if (sysctl(oid_swapusage, 2, &swap, &len, NULL, 0) != 0)
        memset(&swap, 0, sizeof(swap));

    b->ullTotalPhys     = (ULONGLONG)memsize;
    b->ullAvailPhys     = avail;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)swap.xsu_total;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)swap.xsu_avail;
#else
    struct sysinfo si;
    if (sysinfo(&si) != 0) return FALSE;

    ULONGLONG unit = si.mem_unit ? si.mem_unit : 1;
    b->ullTotalPhys     = (ULONGLONG)si.totalram  * unit;
    b->ullAvailPhys     = (ULONGLONG)si.freeram   * unit;
    b->ullTotalPageFile = b->ullTotalPhys + (ULONGLONG)si.totalswap * unit;
    b->ullAvailPageFile = b->ullAvailPhys + (ULONGLONG)si.freeswap  * unit;
#endif
    b->ullTotalVirtual  = b->ullTotalPhys;
    b->ullAvailVirtual  = b->ullAvailPhys;
    b->ullAvailExtendedVirtual = 0;
    b->dwMemoryLoad = b->ullTotalPhys
        ? (DWORD)(100 - (b->ullAvailPhys * 100 / b->ullTotalPhys)) : 0;
    return TRUE;
}

/* ===================================================================== */
/* Aligned allocation                                                     */
/* ===================================================================== */

void *_aligned_malloc(SIZE_T size, SIZE_T alignment)
{
    if (alignment < sizeof(void *)) alignment = sizeof(void *);
    /* round alignment up to a power of two */
    SIZE_T a = sizeof(void *);
    while (a < alignment) a <<= 1;
    void *p = NULL;
    if (posix_memalign(&p, a, size ? size : 1) != 0) return NULL;
    return p;
}

void _aligned_free(void *ptr) { free(ptr); }

/* ===================================================================== */
/* Case-insensitive string compare                                        */
/* ===================================================================== */

int _stricmp(const char *a, const char *b)            { return strcasecmp(a, b); }
int _strnicmp(const char *a, const char *b, SIZE_T n) { return strncasecmp(a, b, n); }

/* ===================================================================== */
/* Wide-string helpers (16-bit Xbox WCHAR)                                */
/* ===================================================================== */

SIZE_T xbox_wcslen(const WCHAR *s)
{
    SIZE_T n = 0;
    if (s) while (s[n]) n++;
    return n;
}

int xbox_wcsncmp(const WCHAR *a, const WCHAR *b, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)a[i] - (int)b[i];
        if (a[i] == 0)    return 0;
    }
    return 0;
}

WCHAR *xbox_wcscat(WCHAR *dst, const WCHAR *src)
{
    SIZE_T d = xbox_wcslen(dst), i = 0;
    while (src[i]) { dst[d + i] = src[i]; i++; }
    dst[d + i] = 0;
    return dst;
}

WCHAR *xbox_wcscpy(WCHAR *dst, const WCHAR *src)
{
    SIZE_T i = 0;
    while (src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
    return dst;
}

/* ===================================================================== */
/* Time conversion                                                        */
/* ===================================================================== */

BOOL SystemTimeToFileTime(const SYSTEMTIME *st, LPFILETIME ft)
{
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = st->wYear - 1900;
    tm.tm_mon  = st->wMonth - 1;
    tm.tm_mday = st->wDay;
    tm.tm_hour = st->wHour;
    tm.tm_min  = st->wMinute;
    tm.tm_sec  = st->wSecond;
    time_t t = timegm(&tm);
    ULONGLONG ticks = FILETIME_EPOCH_DIFF
                    + (ULONGLONG)t * 10000000ULL
                    + (ULONGLONG)st->wMilliseconds * 10000ULL;
    ft->dwLowDateTime  = (DWORD)(ticks & 0xFFFFFFFFULL);
    ft->dwHighDateTime = (DWORD)(ticks >> 32);
    return TRUE;
}

BOOL FileTimeToSystemTime(const FILETIME *ft, LPSYSTEMTIME st)
{
    ULONGLONG ticks = ((ULONGLONG)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
    if (ticks < FILETIME_EPOCH_DIFF) { memset(st, 0, sizeof(*st)); return FALSE; }
    ULONGLONG since = ticks - FILETIME_EPOCH_DIFF;
    time_t t = (time_t)(since / 10000000ULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    fill_systemtime(st, &tm, (long)((since % 10000000ULL) * 100ULL));
    return TRUE;
}

/* ===================================================================== */
/* Exception handling (compile-shim -- SEH not yet emulated on Linux)     */
/* ===================================================================== */

VOID RtlUnwind(PVOID TargetFrame, PVOID TargetIp,
               PEXCEPTION_RECORD ExceptionRecord, PVOID ReturnValue)
{
    (void)TargetFrame; (void)TargetIp; (void)ExceptionRecord; (void)ReturnValue;
    /* TODO: Windows SEH unwinding is not yet emulated on Linux. */
}

VOID RaiseException(DWORD code, DWORD flags, DWORD nargs, const ULONG_PTR *args)
{
    (void)flags; (void)nargs; (void)args;
    fprintf(stderr, "[win32_compat] RaiseException(0x%08X): SEH not emulated\n", code);
    /* TODO: on Windows this does not return; SEH dispatch unimplemented. */
}

PVOID AddVectoredExceptionHandler(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler)
{ (void)First; (void)Handler; return NULL; }   /* TODO: wire to sigaction */
ULONG RemoveVectoredExceptionHandler(PVOID h) { (void)h; return 1; }

#endif /* !_WIN32 */
