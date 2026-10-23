/*
 * d3d8_readback.h -- reading back the presented picture without stalling the frame.
 *
 * IDirect3DSurface8::LockRect on the back buffer copies the picture and then waits
 * for the GPU to finish everything queued so far: on Direct3D 11 the Map of a fresh
 * staging copy blocks, on Vulkan the lock submits the frame so far and waits on its
 * fence. A tool that wants pictures every few frames (a recorder, a capture for a
 * test, a thumbnail) pays that stall each time, in the frame it was taken.
 *
 * Here the copy is started now and collected later:
 *
 *     d3d8_readback_begin(tag)    queue a copy of the picture about to be presented
 *                                 (call after the title has drawn, before Present)
 *     d3d8_readback_poll(&rb)     collect the oldest finished copy, if one is done;
 *                                 never waits
 *
 * Direct3D 11: a ring of staging textures, CopyResource now, Map with
 * D3D11_MAP_FLAG_DO_NOT_WAIT later (a frame or two). Vulkan: the copy is recorded into
 * the frame's own command buffer and completes with the frame's submission, which
 * this device already waits for; no extra submit. The OpenGL device has no
 * asynchronous path and reports itself unsupported (begin returns -1); use LockRect.
 *
 * The picture is what the title drew at its render scale (the target before it is
 * scaled into the window), as a back-buffer LockRect returns it, with no gamma ramp.
 * Everything here runs on the thread that presents. At most D3D8_READBACK_SLOTS
 * copies are in flight; begin returns 0 while all are busy.
 */
#ifndef D3D8_READBACK_H
#define D3D8_READBACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define D3D8_READBACK_SLOTS 3

typedef struct D3D8Readback {
    const uint8_t *pixels;      /* rows of 4-byte pixels; valid until the next poll or begin */
    int      pitch;             /* bytes from one row to the next */
    uint32_t width, height;
    int      bgra;              /* 0: bytes are R, G, B, A; 1: B, G, R, A */
    uint64_t tag;               /* what begin was given */
} D3D8Readback;

/* 1 started, 0 every slot is busy (poll first), -1 not supported by this device
 * (or no device yet, or a multisampled target). */
int d3d8_readback_begin(uint64_t tag);

/* 1 and *out filled with the oldest finished copy; 0 none is finished yet. */
int d3d8_readback_poll(D3D8Readback *out);

/* Forget every copy in flight and free the copies (a device reset, shutdown). */
void d3d8_readback_reset(void);

#ifdef __cplusplus
}
#endif

#endif
