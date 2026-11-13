/* SPDX-License-Identifier: MIT
 *
 * Deterministic math for the simulation (docs/research/deterministic-math.md).
 *
 * The lifted game sends the x87 transcendentals (fsin, fcos, fsincos, fptan, fpatan, fyl2x,
 * fyl2xp1, f2xm1) to C library functions. Every host's libm is a different implementation
 * (MSVC UCRT, with an FMA3 path on newer CPUs; glibc; Apple; Android bionic), none correctly
 * rounded, so the same fight computed on two of them drifts apart by an ulp here and there, and
 * lockstep desyncs. dm_* are one fixed algorithm (the published fdlibm / FreeBSD msun ones,
 * re-derived and checked by tools/detmath/gen_constants.py) built only from IEEE-754 add, sub,
 * mul, div and integer bit operations, with no FMA contraction, so they give the same bits on
 * x86-64, arm64 and any other IEEE double machine.
 *
 * dm_* never touch errno or the floating-point environment and never call the C library, so
 * detmath.c also builds freestanding (tests/detmath: the cross-architecture vector check).
 *
 * NaN results: an invalid operation (sin(inf), log2(-1), ...) returns 0xFFF8000000000000, the
 * x86 default NaN, on every host; a NaN operand comes back quieted with its own payload.
 */
#ifndef DETMATH_H
#define DETMATH_H

#ifdef __cplusplus
extern "C" {
#endif

double dm_sin(double x);
double dm_cos(double x);
double dm_tan(double x);
double dm_atan(double x);
double dm_atan2(double y, double x);
double dm_log(double x);
double dm_log2(double x);
double dm_log1p(double x);
double dm_expm1(double x);

/* The switch the lifted code's calls go through (detmath_select.c; the game only).
 * RECOMP_DETMATH: 1 (default) the dm_* functions, 0 the host libm as before (comparison runs),
 * 2 the dm_* functions and, next to each, the host's answer, counted when it differs
 * ([DETMATH] lines in the log). RECOMP_DETMATH_TRACE=<file> also writes every call's
 * operands there (tests/detmath replays them on other architectures). */
int recomp_detmath_mode(void);
double recomp_libm_sin(double x);
double recomp_libm_cos(double x);
double recomp_libm_tan(double x);
double recomp_libm_atan2(double y, double x);
double recomp_libm_log2(double x);
double recomp_libm_log1p(double x);
double recomp_libm_expm1(double x);

/* Puts the host's floating-point environment back to what the simulation assumes (round to
 * nearest, no flush-to-zero, no denormals-are-zero; on x86 MXCSR 0x1F80, on arm64 FPCR RMode,
 * FZ and DN clear) if anything on this thread changed it. Called once per fight step. Returns 1
 * when it had to repair something. */
int recomp_detmath_fenv_guard(void);

/* Prints the RECOMP_DETMATH=2 comparison counts to stderr (also at exit). */
void recomp_detmath_report(void);

#ifdef __cplusplus
}
#endif

#endif
