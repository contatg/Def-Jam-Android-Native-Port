/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 sweeax-exe (FFNY Online) */
#ifndef DEFJAM_GUEST_WINDOW_H
#define DEFJAM_GUEST_WINDOW_H

#include <stdint.h>

#ifndef XBOX_CONTIG_MB
#define XBOX_CONTIG_MB 64
#endif
#define GUEST_WINDOW_LO    0x80000000u
#define GUEST_WINDOW_BYTES ((uint32_t)(XBOX_CONTIG_MB) << 20)
#define GUEST_WINDOW_HI    (GUEST_WINDOW_LO + GUEST_WINDOW_BYTES)

#endif
