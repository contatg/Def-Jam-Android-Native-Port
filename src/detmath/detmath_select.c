/* SPDX-License-Identifier: MIT
 *
 * The switch between the deterministic math layer and the host libm for the lifted game's
 * transcendental calls (detmath_redirect.h sends them here), its comparison counters, its call
 * trace and the floating-point environment guard. See detmath.h for the env vars.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "detmath.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#define DM_X86 1
#endif
#ifdef _WIN32
#include <windows.h>
#endif

enum { F_SIN, F_COS, F_TAN, F_ATAN, F_ATAN2, F_LOG, F_LOG2, F_LOG1P, F_EXPM1, F_COUNT };
static const char *const g_name[F_COUNT] = {"sin", "cos", "tan", "atan", "atan2", "log", "log2", "log1p", "expm1"};

static volatile long g_mode = -1;

/* ---- a small lock (trace writer, start-up) ------------------------------------------------------ */

static volatile long g_lock;
static void lock(void)
{
#ifdef _WIN32
    while (InterlockedExchange(&g_lock, 1)) YieldProcessor();
#else
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {}
#endif
}
static void unlock(void)
{
#ifdef _WIN32
    InterlockedExchange(&g_lock, 0);
#else
    __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE);
#endif
}

/* ---- RECOMP_DETMATH=2: how often, and by how much, the host's answer differs -------------------- */

static struct { volatile uint64_t calls, differ, nan_bits; volatile uint64_t max_ulp; } g_cmp[F_COUNT];
static volatile uint64_t g_cmp_total;

static uint64_t bits_of(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

static uint64_t ulp_distance(double a, double b)
{
    int64_t x = (int64_t)bits_of(a), y = (int64_t)bits_of(b);
    if (x < 0) x = INT64_MIN - x;
    if (y < 0) y = INT64_MIN - y;
    return x > y ? (uint64_t)(x - y) : (uint64_t)(y - x);
}

static void compare(int f, double det, double host)
{
    g_cmp[f].calls++;
    if (bits_of(det) != bits_of(host)) {
        if (det != det && host != host) g_cmp[f].nan_bits++;
        else {
            uint64_t d = (det != det || host != host) ? UINT64_MAX : ulp_distance(det, host);
            g_cmp[f].differ++;
            if (d > g_cmp[f].max_ulp) g_cmp[f].max_ulp = d;
        }
    }
    if ((++g_cmp_total & ((1u << 18) - 1)) == 0) recomp_detmath_report();   /* every 256K calls (copies are often killed, not exited) */
}

void recomp_detmath_report(void)
{
    int f;
    if (g_mode != 2) return;
    for (f = 0; f < F_COUNT; f++)
        if (g_cmp[f].calls)
            fprintf(stderr, "[DETMATH] %s: %llu calls, %llu differ from the host libm (max %llu ulp), %llu only in NaN bits\n",
                    g_name[f], (unsigned long long)g_cmp[f].calls, (unsigned long long)g_cmp[f].differ,
                    (unsigned long long)g_cmp[f].max_ulp, (unsigned long long)g_cmp[f].nan_bits);
    fflush(stderr);
}

/* ---- RECOMP_DETMATH_TRACE: every call's operands, for tests/detmath --------------------------- */

typedef struct { uint32_t fn, zero; uint64_t a, b; } TraceRec;
#define TRACE_BUF 65536
static FILE *g_trace;
static TraceRec *g_trace_buf;
static unsigned g_trace_n;
static uint64_t g_trace_left;

static void trace_flush_locked(void)
{
    if (g_trace && g_trace_n) { fwrite(g_trace_buf, sizeof(TraceRec), g_trace_n, g_trace); fflush(g_trace); }
    g_trace_n = 0;
}

static void trace(int f, double a, double b)
{
    lock();
    if (g_trace && g_trace_left) {
        TraceRec *r = &g_trace_buf[g_trace_n++];
        r->fn = (uint32_t)f; r->zero = 0; r->a = bits_of(a); r->b = bits_of(b);
        if (--g_trace_left == 0) {
            trace_flush_locked();
            fclose(g_trace);
            g_trace = NULL;
            fprintf(stderr, "[DETMATH] trace full, closed\n");
        } else if (g_trace_n == TRACE_BUF) trace_flush_locked();
    }
    unlock();
}

static void at_exit(void)
{
    recomp_detmath_report();
    lock();
    trace_flush_locked();
    unlock();
}

/* ---- the mode ------------------------------------------------------------------------------------ */

static long mode_init(void)
{
    long m = 1;
    const char *v, *t;
    lock();
    if (g_mode >= 0) { unlock(); return g_mode; }
    v = getenv("RECOMP_DETMATH");
    if (v && *v) m = v[0] == '0' ? 0 : v[0] == '2' ? 2 : 1;
    t = getenv("RECOMP_DETMATH_TRACE");
    if (t && *t) {
        const char *cap = getenv("RECOMP_DETMATH_TRACE_MAX");
        g_trace_left = cap && *cap ? strtoull(cap, NULL, 10) : 4000000u;
        g_trace_buf = (TraceRec *)malloc(sizeof(TraceRec) * TRACE_BUF);
        g_trace = g_trace_buf ? fopen(t, "wb") : NULL;
        fprintf(stderr, "[DETMATH] trace to %s (%s, up to %llu calls)\n", t, g_trace ? "open" : "FAILED",
                (unsigned long long)g_trace_left);
    }
    fprintf(stderr, "[DETMATH] mode %ld: %s\n", m,
            m == 0 ? "host libm (RECOMP_DETMATH=0, comparison only: does not meet mode-1 copies online)"
            : m == 2 ? "deterministic, compared with the host libm" : "deterministic");
    recomp_detmath_fenv_guard();
    atexit(at_exit);
    g_mode = m;
    unlock();
    return m;
}

int recomp_detmath_mode(void)
{
    long m = g_mode;
    return (int)(m >= 0 ? m : mode_init());
}

#define DM_SELECT1(name, id)                                                       \
    double recomp_libm_##name(double x)                                            \
    {                                                                              \
        int m = recomp_detmath_mode();                                             \
        double r;                                                                  \
        if (g_trace) trace(id, x, 0.0);                                            \
        if (m == 0) return name(x);                                                \
        r = dm_##name(x);                                                          \
        if (m == 2) compare(id, r, name(x));                                       \
        return r;                                                                  \
    }

DM_SELECT1(sin, F_SIN)
DM_SELECT1(cos, F_COS)
DM_SELECT1(tan, F_TAN)
DM_SELECT1(log2, F_LOG2)
DM_SELECT1(log1p, F_LOG1P)
DM_SELECT1(expm1, F_EXPM1)

double recomp_libm_atan2(double y, double x)
{
    int m = recomp_detmath_mode();
    double r;
    if (g_trace) trace(F_ATAN2, y, x);
    if (m == 0) return atan2(y, x);
    r = dm_atan2(y, x);
    if (m == 2) compare(F_ATAN2, r, atan2(y, x));
    return r;
}

/* ---- the floating-point environment -------------------------------------------------------------- */

int recomp_detmath_fenv_guard(void)
{
    static volatile long reported;
#if defined(DM_X86)
    /* MXCSR bits 6 (DAZ), 7-12 (exception masks), 13-14 (rounding), 15 (flush to zero) */
    unsigned csr = _mm_getcsr();
    if ((csr & 0xFFC0u) == 0x1F80u) return 0;
    _mm_setcsr((csr & 0x3Fu) | 0x1F80u);
    if (reported++ < 5) fprintf(stderr, "[DETMATH] MXCSR was 0x%04X; reset to 0x1F80 (nearest, no FTZ/DAZ)\n", csr);
    return 1;
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
    /* FPCR bits 22-23 (RMode), 24 (FZ), 25 (DN), 19 (FZ16) */
    uint64_t fpcr, want;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    want = fpcr & ~((3ull << 22) | (1ull << 24) | (1ull << 25) | (1ull << 19));
    if (want == fpcr) return 0;
    __asm__ volatile("msr fpcr, %0" : : "r"(want));
    if (reported++ < 5) fprintf(stderr, "[DETMATH] FPCR was 0x%llx; reset (nearest, no FZ/DN)\n", (unsigned long long)fpcr);
    return 1;
#else
    (void)reported;
    return 0;
#endif
}
