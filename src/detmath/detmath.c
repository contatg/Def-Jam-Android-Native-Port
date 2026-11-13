/* SPDX-License-Identifier: MIT
 *
 * Deterministic sin, cos, tan, atan, atan2, log, log2, log1p and expm1 (see detmath.h).
 *
 * The algorithms and polynomial coefficients are fdlibm's (k_sin, k_cos, k_tan, e_rem_pio2,
 * s_atan, e_atan2, e_log, e_log2 / k_log, s_expm1), in their FreeBSD msun revisions. Their
 * notice, which their terms ask to keep with any copy or derivative:
 *
 * ====================================================
 * Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
 * (k_tan: Copyright 2004 Sun Microsystems, Inc. All Rights Reserved.)
 *
 * Developed at SunPro, a Sun Microsystems, Inc. business.
 * Permission to use, copy, modify, and distribute this
 * software is freely granted, provided that this notice
 * is preserved.
 * ====================================================
 *
 * Our changes: the large-argument reduction is a plain integer Payne-Hanek (2/pi bits from
 * detmath_tables.h, generated with Python integers) instead of __kernel_rem_pio2; the medium
 * reduction always runs fdlibm's cancellation check instead of its npio2_hw shortcut; log1p is
 * log(1+x)*x/((1+x)-1); NaN results are fixed bit patterns (detmath.h); no errno, no flags.
 * Every constant derived from pi or ln 2 and every coefficient spelling is checked by
 * tools/detmath/gen_constants.py.
 *
 * Determinism rules for this file (tests/detmath/check.py proves them per build):
 * - only IEEE-754 double +, -, *, / and int<->double conversions, each rounded once, to
 *   nearest: no fused multiply-add (FP_CONTRACT off below and -ffp-contract=off / MSVC's
 *   /fp:precise in the build), no x87 (FLT_EVAL_METHOD 0), no fast-math;
 * - no C library call and no host long double.
 */
#include <stdint.h>
#include <float.h>
#include "detmath.h"
#include "detmath_tables.h"

#if defined(__FAST_MATH__)
#error "detmath.c must not be built with -ffast-math"
#endif
#if defined(FLT_EVAL_METHOD) && FLT_EVAL_METHOD != 0 && FLT_EVAL_METHOD != -1
#error "detmath.c needs FLT_EVAL_METHOD 0 (SSE2 / NEON doubles, not x87)"
#endif
#if defined(_MSC_VER) && !defined(__clang__)
#pragma fp_contract(off)
#pragma float_control(precise, on)
#elif defined(__clang__)
#pragma STDC FP_CONTRACT OFF
#elif defined(__GNUC__)
#pragma GCC optimize("fp-contract=off")
#endif

/* ---- bits ---------------------------------------------------------------------------------- */

typedef union { double d; uint64_t u; } dm_bits;

static inline uint64_t asu(double d) { dm_bits b; b.d = d; return b.u; }
static inline double asd(uint64_t u) { dm_bits b; b.u = u; return b.d; }
static inline int32_t hiword(double d) { return (int32_t)(uint32_t)(asu(d) >> 32); }
static inline uint32_t loword(double d) { return (uint32_t)asu(d); }
static inline double with_high(double d, uint32_t hi) { return asd(((uint64_t)hi << 32) | (asu(d) & 0xFFFFFFFFu)); }
static inline double low_cleared(double d) { return asd(asu(d) & 0xFFFFFFFF00000000ull); }
static inline double absd(double d) { return asd(asu(d) & 0x7FFFFFFFFFFFFFFFull); }
static inline int is_nan(double d) { return (asu(d) & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }
/* 2^p for -1022 <= p <= 1023, exactly. */
static inline double pow2(int p) { return asd((uint64_t)(1023 + p) << 52); }

#define DM_INF asd(0x7FF0000000000000ull)
/* An invalid operation's result, the same on every host (the x86 default NaN). */
static inline double invalid(void) { return asd(0xFFF8000000000000ull); }
/* A NaN operand, quieted, as an x86 operation would return it. */
static inline double quiet(double x) { return asd(asu(x) | 0x0008000000000000ull); }

/* ---- constants (fdlibm; all checked by tools/detmath/gen_constants.py) ----------------------- */

static const double
    S1 = -1.66666666666666324348e-01, S2 = 8.33333333332248946124e-03,
    S3 = -1.98412698298579493134e-04, S4 = 2.75573137070700676789e-06,
    S5 = -2.50507602534068634195e-08, S6 = 1.58969099521155010221e-10,
    C1 = 4.16666666666666019037e-02, C2 = -1.38888888888741095749e-03,
    C3 = 2.48015872894767294178e-05, C4 = -2.75573143513906633035e-07,
    C5 = 2.08757232129817482790e-09, C6 = -1.13596475577881948265e-11;

static const double T[13] = {
    3.33333333333334091986e-01, 1.33333333333201242699e-01, 5.39682539762260521377e-02,
    2.18694882948595424599e-02, 8.86323982359930005737e-03, 3.59207910759131235356e-03,
    1.45620945432529025516e-03, 5.88041240820264096874e-04, 2.46463134818469906812e-04,
    7.81794442939557092300e-05, 7.14072491382608190305e-05, -1.85586374855275456654e-05,
    2.59073051863633712884e-05,
};
static const double pio4 = 7.85398163397448278999e-01, pio4lo = 3.06161699786838301793e-17;

static const double
    invpio2 = 6.36619772367581382433e-01,
    pio2_1 = 1.57079632673412561417e+00, pio2_1t = 6.07710050650619224932e-11,
    pio2_2 = 6.07710050630396597660e-11, pio2_2t = 2.02226624879595063154e-21,
    pio2_3 = 2.02226624871116645580e-21, pio2_3t = 8.47842766036889956997e-32,
    pio2_hi = 1.57079632679489655800e+00, pio2_lo = 6.12323399573676603587e-17;

static const double atanhi[4] = {
    4.63647609000806093515e-01, 7.85398163397448278999e-01,
    9.82793723247329054082e-01, 1.57079632679489655800e+00,
};
static const double atanlo[4] = {
    2.26987774529616870924e-17, 3.06161699786838301793e-17,
    1.39033110312309984516e-17, 6.12323399573676603587e-17,
};
static const double aT[11] = {
    3.33333333333329318027e-01, -1.99999999998764832476e-01, 1.42857142725034663711e-01,
    -1.11111104054623557880e-01, 9.09088713343650656196e-02, -7.69187620504482999495e-02,
    6.66107313738753120669e-02, -5.83357013379057348645e-02, 4.97687799461593236017e-02,
    -3.65315727442169155270e-02, 1.62858201153657823623e-02,
};
static const double pi = 3.1415926535897931160e+00, pi_lo = 1.2246467991473531772e-16,
                    pi_o_2 = 1.5707963267948965580e+00, pi_o_4 = 7.8539816339744827900e-01;

static const double
    Lg1 = 6.666666666666735130e-01, Lg2 = 3.999999999940941908e-01,
    Lg3 = 2.857142874366239149e-01, Lg4 = 2.222219843214978396e-01,
    Lg5 = 1.818357216161805012e-01, Lg6 = 1.531383769920937332e-01,
    Lg7 = 1.479819860511658591e-01;
static const double
    ln2_hi = 6.93147180369123816490e-01, ln2_lo = 1.90821492927058770002e-10,
    two54 = 1.80143985094819840000e+16,
    ivln2hi = 1.44269504072144627571e+00, ivln2lo = 1.67517131648865118353e-10,
    invln2 = 1.44269504088896338700e+00,
    o_threshold = 7.09782712893383973096e+02;
static const double
    Q1 = -3.33333333333331316428e-02, Q2 = 1.58730158725481460165e-03,
    Q3 = -7.93650757867487942473e-05, Q4 = 4.00821782732936239552e-06,
    Q5 = -2.01099218183624371326e-07;

/* ---- kernels on [-pi/4, pi/4]; x + y is the reduced argument, |y| < ulp(x)/2 ----------------- */

static double k_sin(double x, double y, int iy)
{
    double z = x * x, w = z * z, r, v;
    r = S2 + z * (S3 + z * S4) + z * w * (S5 + z * S6);
    v = z * x;
    if (iy == 0) return x + v * (S1 + z * r);
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static double k_cos(double x, double y)
{
    double z = x * x, w = z * z, r, hz;
    r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    hz = 0.5 * z;
    w = 1.0 - hz;
    return w + (((1.0 - w) - hz) + (z * r - x * y));
}

/* tan (iy = 1) or -1/tan (iy = -1) of x + y. */
static double k_tan(double x, double y, int iy)
{
    double z, r, v, w, s;
    int32_t hx = hiword(x), ix = hx & 0x7fffffff;
    int big = ix >= 0x3FE59428;                 /* |x| >= 0.6744 */
    if (big) {
        if (hx < 0) { x = -x; y = -y; }
        z = pio4 - x;
        w = pio4lo - y;
        x = z + w;
        y = 0.0;
    }
    z = x * x;
    w = z * z;
    r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    s = z * x;
    r = y + z * (s * (r + v) + y);
    r += T[0] * s;
    w = x + r;
    if (big) {
        v = (double)iy;
        return (hx < 0 ? -1.0 : 1.0) * (v - 2.0 * (x - (w * w / (w + v) - r)));
    }
    if (iy == 1) return w;
    {   /* -1/(x + r), accurately */
        double a, t;
        z = low_cleared(w);
        v = r - (z - x);
        t = a = -1.0 / w;
        t = low_cleared(t);
        s = 1.0 + t * z;
        return t + a * (s + t * v);
    }
}

/* ---- argument reduction: x = n * pi/2 + (y[0] + y[1]) --------------------------------------- */

static unsigned two_over_pi_bit(int j)          /* bit j (1 = first after the binary point) */
{
    int k = j - 1;
    return (DM_TWO_OVER_PI[k / 24] >> (23 - k % 24)) & 1u;
}

static uint64_t bits_at(const uint32_t P[8], int pos, int count)   /* bits pos..pos+count-1 */
{
    uint64_t r = 0;
    int k;
    for (k = count - 1; k >= 0; k--) {
        int p = pos + k;
        r = (r << 1) | (p >= 0 && p < 256 ? (P[p >> 5] >> (p & 31)) & 1u : 0u);
    }
    return r;
}

/* Exact product a * b = p + e (Dekker; no FMA). */
static void two_prod(double a, double b, double *p, double *e)
{
    const double split = 134217729.0;           /* 2^27 + 1 */
    double c, ah, al, bh, bl;
    c = split * a; ah = c - (c - a); al = a - ah;
    c = split * b; bh = c - (c - b); bl = b - bh;
    *p = a * b;
    *e = ((ah * bh - *p) + ah * bl + al * bh) + al * bl;
}

/* |x| >= 2^19 * pi/2, finite: Payne-Hanek with integers. x = m * 2^e; the product of m with
 * a 192-bit window of 2/pi starting two bits above x's units gives the quadrant (2 bits) and
 * 190 or more bits of the fraction, of which 128 are used. */
static int rem_pio2_large(double x, double *y)
{
    uint64_t u = asu(x), m, fh, fl;
    int neg = (int)(u >> 63), e = (int)((u >> 52) & 0x7ff) - 1075, j0, s, k, a, b, n, lz, negf = 0;
    uint32_t W[6] = {0, 0, 0, 0, 0, 0}, P[8] = {0, 0, 0, 0, 0, 0, 0, 0}, ml[2];
    double hi, lo, p, pe, t;
    m = (u & 0x000FFFFFFFFFFFFFull) | 0x0010000000000000ull;
    j0 = e - 1 > 1 ? e - 1 : 1;
    for (k = 0; k < 192; k++)                   /* W little-endian: W[0] holds the last bits */
        W[(191 - k) >> 5] |= two_over_pi_bit(j0 + k) << ((191 - k) & 31);
    ml[0] = (uint32_t)m; ml[1] = (uint32_t)(m >> 32);
    for (a = 0; a < 2; a++) {
        uint64_t carry = 0;
        for (b = 0; b < 6; b++) {
            uint64_t t64 = (uint64_t)ml[a] * W[b] + P[a + b] + carry;
            P[a + b] = (uint32_t)t64;
            carry = t64 >> 32;
        }
        P[a + 6] = (uint32_t)carry;
    }
    s = j0 + 191 - e;                           /* P's fraction bits */
    n = (int)bits_at(P, s, 2);
    fh = bits_at(P, s - 64, 64);
    fl = bits_at(P, s - 128, 64);
    if (fh >> 63) {                             /* fraction >= 1/2: the next quadrant, minus */
        n += 1;
        negf = 1;
        fl = ~fl + 1u;
        fh = ~fh + (fl == 0 ? 1u : 0u);
    }
    if (fh == 0 && fl == 0) { y[0] = y[1] = 0.0; }
    else {
        lz = 0;
        while (!(fh >> 63)) { fh = (fh << 1) | (fl >> 63); fl <<= 1; lz++; }
        hi = (double)(fh >> 11) * pow2(-53 - lz);
        lo = (double)(((fh & 0x7FFu) << 42) | (fl >> 22)) * pow2(-106 - lz);
        two_prod(hi, pio2_hi, &p, &pe);
        t = pe + (hi * pio2_lo + lo * pio2_hi);
        y[0] = p + t;
        y[1] = t - (y[0] - p);
        if (negf) { y[0] = -y[0]; y[1] = -y[1]; }
    }
    if (neg) { y[0] = -y[0]; y[1] = -y[1]; n = -n; }
    return n;
}

static int rem_pio2(double x, double *y)
{
    int32_t hx = hiword(x), ix = hx & 0x7fffffff, n, j, i;
    double t, r, w, fn;
    if (ix >= 0x413921fb) return rem_pio2_large(x, y);     /* |x| >= 2^19 * pi/2 */
    t = absd(x);
    n = (int32_t)(t * invpio2 + 0.5);
    fn = (double)n;
    r = t - fn * pio2_1;
    w = fn * pio2_1t;                           /* first round, good to 85 bits */
    j = ix >> 20;
    y[0] = r - w;
    i = j - (int32_t)(((uint32_t)hiword(y[0]) >> 20) & 0x7ff);
    if (i > 16) {                               /* cancellation: second round, 118 bits */
        t = r;
        w = fn * pio2_2;
        r = t - w;
        w = fn * pio2_2t - ((t - r) - w);
        y[0] = r - w;
        i = j - (int32_t)(((uint32_t)hiword(y[0]) >> 20) & 0x7ff);
        if (i > 49) {                           /* third round, 151 bits */
            t = r;
            w = fn * pio2_3;
            r = t - w;
            w = fn * pio2_3t - ((t - r) - w);
            y[0] = r - w;
        }
    }
    y[1] = (r - y[0]) - w;
    if (hx < 0) { y[0] = -y[0]; y[1] = -y[1]; return -n; }
    return n;
}

/* ---- sin, cos, tan --------------------------------------------------------------------------- */

static double trig_nonfinite(double x) { return is_nan(x) ? quiet(x) : invalid(); }

double dm_sin(double x)
{
    double y[2];
    int32_t ix = hiword(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        if (ix < 0x3e500000) return x;          /* |x| < 2^-26 */
        return k_sin(x, 0.0, 0);
    }
    if (ix >= 0x7ff00000) return trig_nonfinite(x);
    switch (rem_pio2(x, y) & 3) {
    case 0:  return k_sin(y[0], y[1], 1);
    case 1:  return k_cos(y[0], y[1]);
    case 2:  return -k_sin(y[0], y[1], 1);
    default: return -k_cos(y[0], y[1]);
    }
}

double dm_cos(double x)
{
    double y[2];
    int32_t ix = hiword(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        if (ix < 0x3e46a09e) return 1.0;        /* |x| < 2^-27 * sqrt(2) */
        return k_cos(x, 0.0);
    }
    if (ix >= 0x7ff00000) return trig_nonfinite(x);
    switch (rem_pio2(x, y) & 3) {
    case 0:  return k_cos(y[0], y[1]);
    case 1:  return -k_sin(y[0], y[1], 1);
    case 2:  return -k_cos(y[0], y[1]);
    default: return k_sin(y[0], y[1], 1);
    }
}

double dm_tan(double x)
{
    double y[2];
    int n;
    int32_t ix = hiword(x) & 0x7fffffff;
    if (ix <= 0x3fe921fb) {
        if (ix < 0x3e400000) return x;          /* |x| < 2^-27 */
        return k_tan(x, 0.0, 1);
    }
    if (ix >= 0x7ff00000) return trig_nonfinite(x);
    n = rem_pio2(x, y);
    return k_tan(y[0], y[1], (n & 1) ? -1 : 1);
}

/* ---- atan, atan2 ----------------------------------------------------------------------------- */

double dm_atan(double x)
{
    double w, s1, s2, z;
    int32_t hx = hiword(x), ix = hx & 0x7fffffff, id;
    if (ix >= 0x44100000) {                     /* |x| >= 2^66 */
        if (is_nan(x)) return quiet(x);
        return hx > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3fdc0000) {                      /* |x| < 0.4375 */
        if (ix < 0x3e400000) return x;          /* |x| < 2^-27 */
        id = -1;
    } else {
        x = absd(x);
        if (ix < 0x3ff30000) {                  /* |x| < 1.1875 */
            if (ix < 0x3fe60000) { id = 0; x = (2.0 * x - 1.0) / (2.0 + x); }   /* 7/16 <= |x| < 11/16 */
            else { id = 1; x = (x - 1.0) / (x + 1.0); }                       /* 11/16 <= |x| < 19/16 */
        } else {
            if (ix < 0x40038000) { id = 2; x = (x - 1.5) / (1.0 + 1.5 * x); } /* |x| < 2.4375 */
            else { id = 3; x = -1.0 / x; }                                    /* 2.4375 <= |x| < 2^66 */
        }
    }
    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0) return x - x * (s1 + s2);
    z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
    return hx < 0 ? -z : z;
}

double dm_atan2(double y, double x)
{
    double z;
    int32_t hx = hiword(x), hy = hiword(y), ix = hx & 0x7fffffff, iy = hy & 0x7fffffff, k, m;
    uint32_t lx = loword(x), ly = loword(y);
    if (is_nan(x)) return quiet(x);
    if (is_nan(y)) return quiet(y);
    if (hx == 0x3ff00000 && lx == 0) return dm_atan(y);    /* x = 1 */
    m = ((hy < 0) ? 1 : 0) | ((hx < 0) ? 2 : 0);            /* 2 * sign(x) + sign(y) */
    if ((iy | ly) == 0) {                       /* y = 0 */
        switch (m) {
        case 0: case 1: return y;
        case 2:  return pi;
        default: return -pi;
        }
    }
    if ((ix | lx) == 0) return hy < 0 ? -pi_o_2 : pi_o_2;   /* x = 0 */
    if (ix == 0x7ff00000) {                     /* x = inf */
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0:  return pi_o_4;
            case 1:  return -pi_o_4;
            case 2:  return 3.0 * pi_o_4;
            default: return -3.0 * pi_o_4;
            }
        }
        switch (m) {
        case 0:  return 0.0;
        case 1:  return -0.0;
        case 2:  return pi;
        default: return -pi;
        }
    }
    if (iy == 0x7ff00000) return hy < 0 ? -pi_o_2 : pi_o_2; /* y = inf */
    k = (iy - ix) >> 20;
    if (k > 60) { z = pi_o_2 + 0.5 * pi_lo; m &= 1; }      /* |y/x| > 2^60 */
    else if (hx < 0 && k < -60) z = 0.0;                    /* 0 > |y|/x > -2^-60 */
    else z = dm_atan(absd(y / x));
    switch (m) {
    case 0:  return z;
    case 1:  return -z;
    case 2:  return pi - (z - pi_lo);
    default: return (z - pi_lo) - pi;
    }
}

/* ---- log, log2, log1p ------------------------------------------------------------------------- */

double dm_log(double x)
{
    double hfsq, f, s, z, R, w, t1, t2, dk;
    int32_t k = 0, hx = hiword(x), i, j;
    uint32_t lx = loword(x);
    if (is_nan(x)) return quiet(x);
    if (hx < 0x00100000) {                      /* x < 2^-1022 */
        if (((hx & 0x7fffffff) | lx) == 0) return -DM_INF;
        if (hx < 0) return invalid();
        k -= 54; x *= two54;                    /* subnormal: scale up */
        hx = hiword(x);
    }
    if (hx >= 0x7ff00000) return x;             /* +inf */
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    i = (hx + 0x95f64) & 0x100000;
    x = with_high(x, (uint32_t)(hx | (i ^ 0x3ff00000)));   /* x or x/2 in [sqrt(2)/2, sqrt(2)) */
    k += i >> 20;
    f = x - 1.0;
    if ((0x000fffff & (2 + hx)) < 3) {          /* -2^-20 <= f < 2^-20 */
        if (f == 0.0) {
            if (k == 0) return 0.0;
            dk = (double)k;
            return dk * ln2_hi + dk * ln2_lo;
        }
        R = f * f * (0.5 - 0.33333333333333333 * f);
        if (k == 0) return f - R;
        dk = (double)k;
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    s = f / (2.0 + f);
    dk = (double)k;
    z = s * s;
    i = hx - 0x6147a;
    w = z * z;
    j = 0x6b851 - hx;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    i |= j;
    R = t2 + t1;
    if (i > 0) {
        hfsq = 0.5 * f * f;
        if (k == 0) return f - (hfsq - s * (hfsq + R));
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    }
    if (k == 0) return f - s * (f - R);
    return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
}

/* log(1 + f) - f + f*f/2 for f in [sqrt(2)/2 - 1, sqrt(2) - 1] (msun k_log1p). */
static double k_log1p(double f)
{
    double hfsq, s, z, R, w, t1, t2;
    s = f / (2.0 + f);
    z = s * s;
    w = z * z;
    t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    R = t2 + t1;
    hfsq = 0.5 * f * f;
    return s * (hfsq + R);
}

double dm_log2(double x)
{
    double f, hfsq, hi, lo, r, val_hi, val_lo, w, y;
    int32_t i, k = 0, hx = hiword(x);
    uint32_t lx = loword(x);
    if (is_nan(x)) return quiet(x);
    if (hx < 0x00100000) {                      /* x < 2^-1022 */
        if (((hx & 0x7fffffff) | lx) == 0) return -DM_INF;
        if (hx < 0) return invalid();
        k -= 54; x *= two54;
        hx = hiword(x);
    }
    if (hx >= 0x7ff00000) return x;             /* +inf */
    if (hx == 0x3ff00000 && loword(x) == 0) return 0.0;
    k += (hx >> 20) - 1023;
    hx &= 0x000fffff;
    i = (hx + 0x95f64) & 0x100000;
    x = with_high(x, (uint32_t)(hx | (i ^ 0x3ff00000)));
    k += i >> 20;
    y = (double)k;
    f = x - 1.0;
    hfsq = 0.5 * f * f;
    r = k_log1p(f);
    hi = low_cleared(f - hfsq);
    lo = (f - hi) - hfsq + r;
    val_hi = hi * ivln2hi;
    val_lo = (lo + hi) * ivln2lo + lo * ivln2hi;
    w = y + val_hi;                             /* spadd(val_hi, val_lo, y) */
    val_lo += (y - w) + val_hi;
    val_hi = w;
    return val_lo + val_hi;
}

double dm_log1p(double x)
{
    double u;
    uint64_t ax = asu(x) & 0x7FFFFFFFFFFFFFFFull;
    if (is_nan(x)) return quiet(x);
    if (x == -1.0) return -DM_INF;
    if (x < -1.0) return invalid();
    if (ax == 0x7FF0000000000000ull) return x;  /* +inf */
    if (ax < 0x3C90000000000000ull) return x;   /* |x| < 2^-54 */
    u = 1.0 + x;
    if (u == 1.0) return x;
    return dm_log(u) * (x / (u - 1.0));
}

/* ---- expm1 ------------------------------------------------------------------------------------- */

double dm_expm1(double x)
{
    double y, hi, lo, c = 0.0, t, e, hxs, hfx, r1, twopk;
    int32_t k, xsb;
    uint32_t hx = (uint32_t)hiword(x);
    xsb = (int32_t)(hx & 0x80000000u);
    hx &= 0x7fffffffu;
    if (hx >= 0x4043687Au) {                    /* |x| >= 56 ln 2 */
        if (hx >= 0x40862E42u) {                /* |x| >= 709.78 */
            if (hx >= 0x7ff00000u) {
                if (is_nan(x)) return quiet(x);
                return xsb == 0 ? x : -1.0;     /* expm1(+-inf) = inf, -1 */
            }
            if (x > o_threshold) return DM_INF;
        }
        if (xsb != 0) return -1.0;              /* x < -56 ln 2 */
    }
    if (hx > 0x3fd62e42u) {                     /* |x| > ln2 / 2 */
        if (hx < 0x3FF0A2B2u) {                 /* and |x| < 1.5 ln 2 */
            if (xsb == 0) { hi = x - ln2_hi; lo = ln2_lo; k = 1; }
            else { hi = x + ln2_hi; lo = -ln2_lo; k = -1; }
        } else {
            k = (int32_t)(invln2 * x + (xsb == 0 ? 0.5 : -0.5));
            t = (double)k;
            hi = x - t * ln2_hi;                /* exact */
            lo = t * ln2_lo;
        }
        x = hi - lo;
        c = (hi - x) - lo;
    } else if (hx < 0x3c900000u) {              /* |x| < 2^-54 */
        return x;
    } else {
        k = 0;
    }
    hfx = 0.5 * x;
    hxs = x * hfx;
    r1 = 1.0 + hxs * (Q1 + hxs * (Q2 + hxs * (Q3 + hxs * (Q4 + hxs * Q5))));
    t = 3.0 - r1 * hfx;
    e = hxs * ((r1 - t) / (6.0 - x * t));
    if (k == 0) return x - (x * e - hxs);
    twopk = asd((uint64_t)(uint32_t)(0x3ff00000 + k * 0x100000) << 32);  /* 2^k (k = 1024 unused) */
    e = (x * (e - c) - c);
    e -= hxs;
    if (k == -1) return 0.5 * (x - e) - 0.5;
    if (k == 1) {
        if (x < -0.25) return -2.0 * (e - (x + 0.5));
        return 1.0 + 2.0 * (x - e);
    }
    if (k <= -2 || k > 56) {                    /* exp(x) - 1 = exp(x) - 1 directly */
        y = 1.0 - (e - x);
        if (k == 1024) y = y * 2.0 * pow2(1023);
        else y = y * twopk;
        return y - 1.0;
    }
    if (k < 20) {
        t = asd((uint64_t)(uint32_t)(0x3ff00000 - (0x200000 >> k)) << 32);  /* 1 - 2^-k */
        y = t - (e - x);
        y = y * twopk;
    } else {
        t = asd((uint64_t)(uint32_t)((0x3ff - k) * 0x100000) << 32);        /* 2^-k */
        y = x - (e + t);
        y += 1.0;
        y = y * twopk;
    }
    return y;
}
