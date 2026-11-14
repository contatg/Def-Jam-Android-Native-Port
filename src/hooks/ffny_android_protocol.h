/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 sweeax-exe (FFNY Online)
 * Packet layout from the Android client specification. The fingerprint
 * follows the FFNY Online Android specification.
 */
#ifndef DEFJAM_FFNY_ANDROID_PROTOCOL_H
#define DEFJAM_FFNY_ANDROID_PROTOCOL_H

#include <stdint.h>
#include <string.h>

#ifndef FFNY_ANDROID_BUILD_NUMBER
#error Define FFNY_ANDROID_BUILD_NUMBER to the Android APK versionCode
#endif
#define FFNY_ANDROID_PROTOCOL_VERSION "android-1.5"
#define FFNY_ANDROID_LOCKSTEP_RULES 4u
#define FFNY_ANDROID_PLATFORM 1u
#define FFNY_ANDROID_CAPS 0u
#define FFNY_ANDROID_NAME_BYTES 24u
#define FFNY_ANDROID_HELLO_MAX (31u + FFNY_ANDROID_NAME_BYTES)

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
        h1 = (h1 ^ (uint8_t)((uint32_t)FFNY_ANDROID_BUILD_NUMBER >> (8 * i))) * 16777619u;
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
    ffny_android_put32(out, FFNY_ANDROID_BUILD_NUMBER);
    ffny_android_put32(out + 4, content);
    out[8] = FFNY_ANDROID_PLATFORM;
}

static inline size_t ffny_android_lobby_hello(uint8_t out[FFNY_ANDROID_HELLO_MAX],
                                             const char *name, size_t name_len,
                                             uint32_t content)
{
    static const uint8_t prefix[] = { 'D', 'J', 'N', 'P', 1, 1, 2, 0, 0, 0, 0, 0, 0 };
    if (!name || name_len == 0 || name_len > FFNY_ANDROID_NAME_BYTES)
        return 0;
    memcpy(out, prefix, sizeof(prefix));
    ffny_android_fingerprint(out + 13, content);
    out[21] = (uint8_t)name_len;
    memcpy(out + 22, name, name_len);
    ffny_android_hello_suffix(out + 22 + name_len, content);
    return 31 + name_len;
}

static inline void ffny_android_caps_packet(uint8_t out[18], const uint8_t token[8])
{
    static const uint8_t prefix[] = { 'D', 'J', 'N', 'P', 1, 0x20 };
    memcpy(out, prefix, sizeof(prefix));
    memcpy(out + 6, token, 8);
    ffny_android_put32(out + 14, FFNY_ANDROID_CAPS);
}

/* T_PC's body after the eight-byte relay token. */
static inline void ffny_android_device_body(uint8_t out[17], const uint8_t device[16])
{
    out[0] = 16;
    memcpy(out + 1, device, 16);
}

#endif
