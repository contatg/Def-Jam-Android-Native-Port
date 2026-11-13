/* SPDX-License-Identifier: MIT
 *
 * Force-included (CMakeLists.txt: /FI or -include) ahead of every lifted chunk and
 * src/recomp_manual.c. It pulls in the headers recomp_types.h uses, then renames the C library
 * transcendentals the lifter emits (fsin/fcos/fsincos/fptan through recomp_x87_*, fpatan,
 * fyl2x, fyl2xp1, f2xm1) to the switch in detmath_select.c. The toolkit and the lifted code
 * stay as they are; RECOMP_DETMATH=0 gives the host libm back for comparison runs.
 *
 * Exact operations (sqrt, fabs, floor, ceil, trunc, fmod, remquo, frexp, ldexp, scalbn,
 * copysign) are not renamed: IEEE 754 and C define their results to the bit on every host.
 */
#ifndef DETMATH_REDIRECT_H
#define DETMATH_REDIRECT_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#include <emmintrin.h>
#endif
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "detmath.h"

#define sin(x)      recomp_libm_sin(x)
#define cos(x)      recomp_libm_cos(x)
#define tan(x)      recomp_libm_tan(x)
#define atan2(y, x) recomp_libm_atan2((y), (x))
#define log2(x)     recomp_libm_log2(x)
#define log1p(x)    recomp_libm_log1p(x)
#define expm1(x)    recomp_libm_expm1(x)

#endif
