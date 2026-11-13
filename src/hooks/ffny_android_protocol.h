/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 sweeax-exe (FFNY Online)
 * PC fingerprint and packet layout adapted from FFNY Online build 78.
 */
#ifndef DEFJAM_FFNY_ANDROID_PROTOCOL_H
#define DEFJAM_FFNY_ANDROID_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#define FFNY_ANDROID_PROTOCOL_BUILD 78u
#define FFNY_ANDROID_PROTOCOL_VERSION "0.13.0"
#define FFNY_ANDROID_LOCKSTEP_RULES 4u
#define FFNY_ANDROID_PLATFORM 1u
#define FFNY_ANDROID_CAPS 0u

static inline uint32_t ffny_android_mix(uint32_t x)
{
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    return x ^ (x >> 16);
}

static inline void ffny_android_put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (8 * i));
}

static inline void ffny_android_fingerprint(uint8_t out[8], uint32_t content)
{
    uint32_t h1 = 2166136261u;
    for (const char *v = FFNY_ANDROID_PROTOCOL_VERSION; *v; ++v)
        h1 = (h1 ^ (uint8_t)*v) * 16777619u;
    for (unsigned i = 0; i < 4; ++i)
        h1 = (h1 ^ (uint8_t)(FFNY_ANDROID_PROTOCOL_BUILD >> (8 * i))) * 16777619u;
    uint32_t h2 = ffny_android_mix(h1 ^ FFNY_ANDROID_LOCKSTEP_RULES);
    if (content) {
        h1 = ffny_android_mix(h1 ^ content);
        h2 = ffny_android_mix(h2 ^ content ^ 0x434F4E54u);
    }
    ffny_android_put32(out, ffny_android_mix(h1 ^ 0x414E4452u));
    ffny_android_put32(out + 4, ffny_android_mix(h2 ^ 0x414E4452u));
}

/* Append to the PC HELLO, immediately after the name. */
static inline void ffny_android_hello_suffix(uint8_t out[9], uint32_t content)
{
    ffny_android_put32(out, FFNY_ANDROID_PROTOCOL_BUILD);
    ffny_android_put32(out + 4, content);
    out[8] = FFNY_ANDROID_PLATFORM;
}

/* T_PC's body after the eight-byte relay token. */
static inline void ffny_android_device_body(uint8_t out[17], const uint8_t device[16])
{
    out[0] = 16;
    memcpy(out + 1, device, 16);
}

#endif
