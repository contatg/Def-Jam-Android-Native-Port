/*
 * MCPX APU Core - Standalone extraction from xemu
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2018-2019 Jannik Vogel
 * Copyright (c) 2019-2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "apu_state.h"
#ifndef _WIN32
#include <unistd.h>
#endif
#include "apu.h"
#include "apu_xaudio2.h"
#include "fpconv.h"
#include "../kernel/xbox_memory_layout.h"   /* XBOX_WORKER_STACK_TOP */
#if defined(__APPLE__)
#include <pthread/qos.h>
#endif

/* ============================================================
 * Globals
 * ============================================================ */

uint8_t *g_apu_ram_ptr = NULL;

MCPXAPUState *g_state = NULL;

/* Forward declarations for software mixer */
static void mixer_init(void);
static void mixer_render(int16_t frame_buf[][2], int num_samples);
static APUMixerVoice g_mixer_voices[APU_MIXER_MAX_VOICES];
static volatile int g_mixer_active_count = 0;
static CRITICAL_SECTION g_mixer_cs;
static bool g_mixer_initialized = false;
struct McpxApuDebug g_dbg;
struct McpxApuDebug g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4] = { 0 };

/* Global audio mute — disables all AWD/mixer sound playback */
volatile int g_audio_muted = 0;  /* 0 = audio enabled */

/* ============================================================
 * Debug frame markers (minimal stubs)
 * ============================================================ */

void mcpx_debug_begin_frame(void) {}
void mcpx_debug_end_frame(void) {}

/* ============================================================
 * Physical addresses
 *
 * The APU is handed physical addresses -- the voice array, the notifier
 * block, the scatter-gather lists and every page of sample data -- and the
 * model used to read physical P at guest address P. That is low RAM, and
 * DirectSound allocates all of those from the contiguous window, which here is
 * separate storage at 0x80000000 + P (xbox_memory_layout.c says why). So the
 * model and the title each kept their own copy of every voice: the model
 * played voices the title could not see move, a stream's position never
 * advanced where the title read it, and a title that paces its mixer on that
 * position mixed nothing. Def Jam's EA audio library does exactly that, so a
 * movie's sound was never consumed and the movie never finished.
 *
 * The kernel records every translation it hands out (xbox_PhysicalToVirtual)
 * and that answer is exact; an address it never handed out is taken to be in
 * the contiguous window, the same rule the OHCI model uses. The lookup is a
 * linear scan and the voice processor makes half a million accesses a
 * second, so answers are cached per page and dropped when the kernel's map
 * changes.
 * ============================================================ */

extern uint32_t xbox_PhysicalToVirtual(uint32_t pa);
extern uint32_t xbox_PhysMapGeneration(void);

#define APU_PHYS_CACHE 256
#define APU_CONTIG_BASE 0x80000000u

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    /* One 64-bit word per entry so a reader on another thread sees a whole
     * entry or none: generation (24 bits, offset by one so a zeroed entry
     * never matches), physical page (20), guest page (20). */
    static volatile uint64_t cache[APU_PHYS_CACHE];
    uint32_t pa = (uint32_t)addr & 0x03FFFFFFu;
    uint32_t pn = pa >> 12;
    uint64_t tag = ((uint64_t)((xbox_PhysMapGeneration() + 1) & 0xFFFFFFu) << 40)
                 | ((uint64_t)pn << 20);
#ifdef _MSC_VER
    uint64_t e = (uint64_t)_InterlockedCompareExchange64(
        (volatile __int64 *)&cache[pn % APU_PHYS_CACHE], 0, 0);
#else
    uint64_t e = __atomic_load_n(&cache[pn % APU_PHYS_CACHE], __ATOMIC_ACQUIRE);
#endif
    uint32_t vn;

    if ((e & ~(uint64_t)0xFFFFF) == tag) {
        vn = (uint32_t)(e & 0xFFFFF);
    } else {
        uint32_t va = xbox_PhysicalToVirtual(pa);
        if (!va)
            va = APU_CONTIG_BASE + pa;
        vn = va >> 12;
#ifdef _MSC_VER
        _InterlockedExchange64((volatile __int64 *)&cache[pn % APU_PHYS_CACHE],
                               (__int64)(tag | vn));
#else
        __atomic_store_n(&cache[pn % APU_PHYS_CACHE], tag | vn, __ATOMIC_RELEASE);
#endif
    }
    return g_apu_ram_ptr + ((uintptr_t)vn << 12) + (pa & 0xFFFu);
}

/* ============================================================
 * IRQ handling (stubbed - no PCI bus in standalone)
 * ============================================================ */

/* Level-triggered APU line; delivered by the kernel timer's vector-5 path. */
static volatile LONG s_irq_line;
int xbox_ApuIrqPending(void)
{
    return InterlockedCompareExchange(&s_irq_line, 0, 0) != 0;
}
static void update_irq(MCPXAPUState *d)
{
    if (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_FETINTSTS);
    }
    if ((d->regs[NV_PAPU_IEN] & NV_PAPU_ISTS_GINTSTS) &&
        ((d->regs[NV_PAPU_ISTS] & ~NV_PAPU_ISTS_GINTSTS) &
         d->regs[NV_PAPU_IEN])) {
        qatomic_or(&d->regs[NV_PAPU_ISTS], NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 1);
        pci_irq_assert(PCI_DEVICE(d));
    } else {
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~NV_PAPU_ISTS_GINTSTS);
        InterlockedExchange(&s_irq_line, 0);
        pci_irq_deassert(PCI_DEVICE(d));
    }
}

/* ============================================================
 * MMIO Read / Write
 * ============================================================ */

uint64_t mcpx_apu_read(void *opaque, hwaddr addr, unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;
    uint64_t r = 0;

    switch (addr) {
    case NV_PAPU_XGSCNT:
        r = (uint64_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 100);
        break;
    default:
        if (addr < 0x20000) {
            r = qatomic_read(&d->regs[addr]);
        }
        break;
    }

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] read  [0x%05llX] size=%u -> 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)r);
     */
    (void)size;
    return r;
}

void mcpx_apu_write(void *opaque, hwaddr addr, uint64_t val,
                     unsigned int size)
{
    MCPXAPUState *d = (MCPXAPUState *)opaque;

    /* Uncomment for register tracing:
     * fprintf(stderr, "[APU] write [0x%05llX] size=%u <- 0x%08llX\n",
     *         (unsigned long long)addr, size, (unsigned long long)val);
     */
    (void)size;

    switch (addr) {
    case NV_PAPU_ISTS:
        qatomic_and(&d->regs[NV_PAPU_ISTS], ~(uint32_t)val);
        update_irq(d);
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FECTL:
    case NV_PAPU_SECTL:
        qatomic_set(&d->regs[addr], (uint32_t)val);
        /* Starting the APU has to start the frame thread.
         *
         * The thread idles on pause_requested, which init sets and only the
         * test tone ever cleared -- so a title that enabled the APU through
         * these registers got an APU that stayed asleep. Nothing then advanced
         * the front end, and a title waiting on a notify completion (the
         * FEMEMDATA magic write, which is how completion reaches guest memory)
         * waited forever. Wreckless hangs exactly there during DirectSound
         * init, and because it initialises its whole engine behind a
         * successful DirectSound create, that hang is not confined to audio.
         *
         * Resume whenever the write is not switching the block off; the thread
         * re-checks FECTL itself and idles again if it is halted or trapped. */
        {
            uint32_t sectl = qatomic_read(&d->regs[NV_PAPU_SECTL]);
            uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
            bool running =
                ((sectl & NV_PAPU_SECTL_XCNTMODE) != NV_PAPU_SECTL_XCNTMODE_OFF)
                && ((fectl & NV_PAPU_FECTL_FEMETHMODE)
                    != NV_PAPU_FECTL_FEMETHMODE_HALTED);
            if (running && d->pause_requested) {
                d->pause_requested = false;
                fprintf(stderr, "[APU] started by the title"
                                " (SECTL=%08X FECTL=%08X)\n", sectl, fectl);
            }
        }
        qemu_cond_broadcast(&d->cond);
        break;
    case NV_PAPU_FEMEMDATA:
        /* 'magic write' - value written to FEMEMADDR on notify completion */
        stl_le_phys(address_space_memory, d->regs[NV_PAPU_FEMEMADDR], (uint32_t)val);
        qatomic_set(&d->regs[addr], (uint32_t)val);
        break;
    default:
        if (addr < 0x20000) {
            qatomic_set(&d->regs[addr], (uint32_t)val);
        }
        break;
    }
}

/* ============================================================
 * Test tone state (used by monitor and test tone functions)
 * ============================================================ */

static struct {
    bool active;
    double phase;
    double phase_inc;
    int16_t amplitude;
} g_test_tone = { false, 0.0, 0.0, 0 };

/* ============================================================
 * Monitor - Audio output (XAudio2 primary, waveOut fallback)
 * ============================================================ */

#if defined(_WIN32)
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif
/* On Linux, waveOut* are inert stubs from win32_compat.h: the APU's
 * waveOut fallback path stays inactive and never produces audio. */

/* Ring of waveOut buffers for double-buffering */
#define WAVEOUT_NUM_BUFS 4
#define WAVEOUT_BUF_SAMPLES 2048  /* ~42.7ms at 48kHz, matches 8-frame delivery rate */
#define MIXER_FRAME_SAMPLES 256  /* Internal mixing frame size (matches frame_buf) */

typedef struct {
    HWAVEOUT hwo;
    WAVEHDR  hdrs[WAVEOUT_NUM_BUFS];
    int16_t  bufs[WAVEOUT_NUM_BUFS][WAVEOUT_BUF_SAMPLES][2];
    int      next_buf;
    bool     initialized;
    int      frames_written;
} WaveOutState;

static WaveOutState g_waveout = { 0 };

void mcpx_apu_monitor_init(MCPXAPUState *d, Error **errp)
{
    (void)errp;
    d->monitor.stream = NULL;
    d->monitor.queued_bytes_low = 1024;
    d->monitor.queued_bytes_high = 3072;

    /* Try XAudio2 first (lower latency) */
    if (xa2_init()) {
        fprintf(stderr, "[APU] Using XAudio2 audio backend\n");
        return;
    }
    fprintf(stderr, "[APU] XAudio2 unavailable, falling back to waveOut\n");

    WAVEFORMATEX wfx = { 0 };
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 2;
    wfx.nSamplesPerSec  = 48000;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = wfx.nChannels * wfx.wBitsPerSample / 8;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

    MMRESULT mr = waveOutOpen(&g_waveout.hwo, WAVE_MAPPER, &wfx,
                               0, 0, CALLBACK_NULL);
    if (mr != MMSYSERR_NOERROR) {
        fprintf(stderr, "[APU] waveOutOpen failed (error %u)\n", mr);
        g_waveout.initialized = false;
        return;
    }

    /* Prepare all headers */
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        memset(&g_waveout.hdrs[i], 0, sizeof(WAVEHDR));
        g_waveout.hdrs[i].lpData = (LPSTR)g_waveout.bufs[i];
        g_waveout.hdrs[i].dwBufferLength = WAVEOUT_BUF_SAMPLES * 2 * sizeof(int16_t);
        waveOutPrepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }

    g_waveout.next_buf = 0;
    g_waveout.initialized = true;
    g_waveout.frames_written = 0;

    fprintf(stderr, "[APU] waveOut audio output initialized (48kHz stereo 16-bit, %d buffers)\n",
            WAVEOUT_NUM_BUFS);
}

void mcpx_apu_monitor_finalize(MCPXAPUState *d)
{
    (void)d;
    if (xa2_is_active()) {
        xa2_shutdown();
        return;
    }
    if (!g_waveout.initialized) return;

    waveOutReset(g_waveout.hwo);
    for (int i = 0; i < WAVEOUT_NUM_BUFS; i++) {
        waveOutUnprepareHeader(g_waveout.hwo, &g_waveout.hdrs[i], sizeof(WAVEHDR));
    }
    waveOutClose(g_waveout.hwo);
    g_waveout.initialized = false;
    fprintf(stderr, "[APU] waveOut audio output shut down (%d frames written)\n",
            g_waveout.frames_written);
}

extern unsigned g_apu_trap_stalls, g_apu_trap_expired;
extern int64_t  g_apu_trap_stall_us;

void mcpx_apu_monitor_frame(MCPXAPUState *d)
{
    if ((d->ep_frame_div + 1) % 8) {
        return;
    }

    /* XAudio2 path.
     *
     * frame_buf holds the eight 32-sample slices the DSP stage wrote this
     * period (mcpx_apu_dsp_frame): the title's own voices. This used to be
     * zeroed here and only the test tone and the software mixer rendered into
     * it, so everything the emulated APU produced was thrown away and a title
     * that played sound through DirectSound was silent. It also submitted
     * 1,024 samples for every 256 produced. Now each period's 256 samples, the
     * software mixer added on top, go into an accumulator that is submitted
     * when it holds one XAudio2 buffer.
     *
     * RECOMP_APU_LEVEL=1 prints the peak of each second of output, which tells
     * "silent" from "not heard" without a listener. */
    if (xa2_is_active()) {
        static int16_t acc[1024][2];      /* matches XA2_BUF_SAMPLES max */
        static int acc_n;
        static int level = -1;
        static int peak, periods;
        int buf_size = xa2_get_buffer_size();
        int chunk = MIXER_FRAME_SAMPLES, i;

        if (buf_size > 1024)
            buf_size = 1024;
        if (level < 0) {
            extern int g_apu_route_on;
            level = getenv("RECOMP_APU_LEVEL") != NULL;
            g_apu_route_on = level;
        }
        if (g_audio_muted)
            memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));
        if (g_test_tone.active && !g_audio_muted) {
            for (i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }
        if (!g_audio_muted)
            mixer_render(d->monitor.frame_buf, chunk);

        for (i = 0; i < chunk && acc_n < buf_size; i++, acc_n++) {
            acc[acc_n][0] = d->monitor.frame_buf[i][0];
            acc[acc_n][1] = d->monitor.frame_buf[i][1];
            if (level) {
                int a = abs(acc[acc_n][0]), b = abs(acc[acc_n][1]);
                if (a > peak) peak = a;
                if (b > peak) peak = b;
            }
        }
        /* The next period's slices are written, not accumulated, by the DSP
         * stage; clear so a period with the pipeline off plays silence. */
        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

        if (acc_n >= buf_size) {
            /* RECOMP_APU_PCM=<file>: the same samples as raw 48 kHz stereo
             * 16-bit PCM, for checking output without a listener (local only;
             * never commit a capture). */
            static FILE *pcm;
            static int pcm_tried;
            if (!pcm_tried) {
                const char *path = getenv("RECOMP_APU_PCM");
                pcm_tried = 1;
                if (path && *path)
                    pcm = fopen(path, "wb");
            }
            if (pcm)
                fwrite(acc, sizeof acc[0], (size_t)buf_size, pcm);
            xa2_submit_samples((const int16_t *)acc, buf_size);
            acc_n = 0;
        }
        if (level && ++periods >= 48000 / MIXER_FRAME_SAMPLES) {
            static int lines;
            if (lines++ < 600)
            {
                extern int mcpx_apu_vp_count_voices(MCPXAPUState *d);
                extern int g_xa2_dropped, g_xa2_starved;
                fprintf(stderr, "[APU] output peak %d over the last second; %d voices,"
                                " SECTL %08X FECTL %08X; buffers dropped %d, device ran dry %d\n",
                        peak, mcpx_apu_vp_count_voices(d),
                        d->regs[NV_PAPU_SECTL], d->regs[NV_PAPU_FECTL],
                        g_xa2_dropped, g_xa2_starved);
                if (lines % 10 == 0)
                    fprintf(stderr, "[APU] front-end stops waited out: %u (%u outlasted"
                                    " the wait), %lld ms in all\n",
                            g_apu_trap_stalls, g_apu_trap_expired,
                            (long long)(g_apu_trap_stall_us / 1000));
                g_xa2_dropped = g_xa2_starved = 0;
            }
            if (lines % 5 == 1 && lines < 120) {
                extern void mcpx_apu_vp_dump_voices(MCPXAPUState *d);
                extern void mcpx_apu_vp_dump_routing(void);
                mcpx_apu_vp_dump_voices(d);
                mcpx_apu_vp_dump_routing();
            }
            peak = 0;
            periods = 0;
        }
        return;
    }

    if (!g_waveout.initialized) return;

    int idx = g_waveout.next_buf;
    WAVEHDR *hdr = &g_waveout.hdrs[idx];

    /* Wait if this buffer is still playing (with timeout) */
    int wait_loops = 0;
    while (!(hdr->dwFlags & WHDR_DONE) && (hdr->dwFlags & WHDR_INQUEUE)) {
        qemu_mutex_unlock(&d->lock);
        Sleep(1);
        qemu_mutex_lock(&d->lock);
        if (++wait_loops > 50) break;
    }

    /* Fill the large waveOut buffer by rendering multiple 256-sample frames */
    int16_t *out = (int16_t *)g_waveout.bufs[idx];
    int remaining = WAVEOUT_BUF_SAMPLES;
    int out_offset = 0;

    while (remaining > 0) {
        int chunk = (remaining < MIXER_FRAME_SAMPLES) ? remaining : MIXER_FRAME_SAMPLES;

        memset(d->monitor.frame_buf, 0, sizeof(d->monitor.frame_buf));

        /* Test tone (skip if muted) */
        if (g_test_tone.active && !g_audio_muted) {
            for (int i = 0; i < chunk; i++) {
                int16_t s = (int16_t)(sin(g_test_tone.phase) * g_test_tone.amplitude);
                d->monitor.frame_buf[i][0] = s;
                d->monitor.frame_buf[i][1] = s;
                g_test_tone.phase += g_test_tone.phase_inc;
                if (g_test_tone.phase >= 2.0 * M_PI)
                    g_test_tone.phase -= 2.0 * M_PI;
            }
        }

        /* Mix software voices (skip if muted) */
        if (!g_audio_muted)
            mixer_render(d->monitor.frame_buf, chunk);

        /* Copy to waveOut buffer */
        memcpy(out + out_offset * 2, d->monitor.frame_buf, chunk * 2 * sizeof(int16_t));
        out_offset += chunk;
        remaining -= chunk;
    }

    /* Submit to waveOut */
    hdr->dwFlags &= ~WHDR_DONE;
    waveOutWrite(g_waveout.hwo, hdr, sizeof(WAVEHDR));

    g_waveout.next_buf = (idx + 1) % WAVEOUT_NUM_BUFS;
    g_waveout.frames_written++;
}

/* ============================================================
 * Throttle (timing control for frame pacing)
 * ============================================================ */

/* The eight 32-sample slices of a period used to run back to back and the thread then
 * slept out the rest of its 5.33 ms, so a voice's play position (CBO) jumped 256 samples
 * at once where hardware advances 32 every 0.67 ms. Measured in 200 s of the opening
 * scene, the game's mixing buffer (voices 0xF8-0xFA) played back the previous lap's
 * samples (the title had not written them yet) about 4.3 times a second, and about 2.2
 * with each slice at its own moment. That is half of them, not all; what drives the rest
 * is not found. Waits shorter than a millisecond need a sleep the host's condition
 * variable does not have, so this is for POSIX hosts: on by default on Linux and macOS
 * (measured on both), RECOMP_APU_SLICE_PACE=1/0 to choose, and off on Windows. */
static int slice_pace_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_APU_SLICE_PACE");
#if defined(_WIN32)
        on = 0;
#else
        on = !(e && *e == '0');
#endif
    }
    return on;
}

static void throttle(MCPXAPUState *d)
{
    static int64_t period_base_us;
    if (d->ep_frame_div % 8) {
#ifndef _WIN32
        if (slice_pace_enabled() && period_base_us && !d->pause_requested) {
            int64_t due = period_base_us + (int64_t)(d->ep_frame_div % 8) * EP_FRAME_US / 8;
            for (;;) {
                int64_t rem = due - qemu_clock_get_us(QEMU_CLOCK_REALTIME);
                if (rem <= 0 || d->pause_requested)
                    break;
                if (rem >= 2000) {
                    qemu_cond_timedwait(&d->cond, &d->lock, (int)(rem / 1000) - 1);
                } else {
                    qemu_mutex_unlock(&d->lock);
                    usleep((useconds_t)rem);
                    qemu_mutex_lock(&d->lock);
                }
            }
        }
#endif
        return;
    }

    int64_t now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);

    /* Up to four periods late is made up, which covers the front-end traps
     * the frame thread waits out; later than that and the clock starts again. */
    if (d->next_frame_time_us == 0 ||
        now_us - d->next_frame_time_us > 4 * EP_FRAME_US) {
        d->next_frame_time_us = now_us;
    }

    while (!d->pause_requested) {
        now_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
        int64_t remaining_ms = (d->next_frame_time_us - now_us) / 1000;
        if (remaining_ms > 0) {
            qemu_cond_timedwait(&d->cond, &d->lock, (int)remaining_ms);
        } else {
            break;
        }
    }
    period_base_us = d->next_frame_time_us;
    d->next_frame_time_us += EP_FRAME_US;
    /* A pause request cuts the wait short, and a frame thread kept running by
     * the mixer while paused comes back here at once: the deadline then ran
     * ahead of the clock by a period per pass, and the first frame after the
     * pause waited it out -- 9.3 s was seen, with the title's audio clock and
     * an audio-paced movie stopped behind it. Never more than four periods
     * ahead, the counterpart of the catch-up limit above. */
    if (d->next_frame_time_us > now_us + 4 * EP_FRAME_US)
        d->next_frame_time_us = now_us + EP_FRAME_US;

    d->sleep_acc_us += (int)(qemu_clock_get_us(QEMU_CLOCK_REALTIME) - now_us);
}

/* ============================================================
 * se_frame - Process one audio frame (VP -> GP -> EP pipeline)
 * ============================================================ */

static void se_frame(MCPXAPUState *d)
{
    mcpx_apu_update_dsp_preference(d);
    mcpx_debug_begin_frame();
    g_dbg.gp_realtime = d->gp.realtime;
    g_dbg.ep_realtime = d->ep.realtime;

    int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    int64_t elapsed_ms = now_ms - d->frame_count_time_ms;
    if (elapsed_ms >= 1000) {
        g_dbg.utilization = 1.0f - d->sleep_acc_us / (elapsed_ms * 1000.0f);
        g_dbg.frames_processed = (int)(d->frame_count * 1000.0 / elapsed_ms + 0.5);
        d->frame_count_time_ms = now_ms;
        d->frame_count = 0;
        d->sleep_acc_us = 0;
    }
    d->frame_count++;

    /* Buffer for all mixbins for this frame */
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    memset(mixbins, 0, sizeof(mixbins));

    mcpx_apu_vp_frame(d, mixbins);
    mcpx_apu_dsp_frame(d, mixbins);
    mcpx_apu_monitor_frame(d);

    d->ep_frame_div++;

    mcpx_debug_end_frame();
}

/* ============================================================
 * APU frame thread (background processing)
 * ============================================================ */

void mcpx_apu_vp_dump_voices(MCPXAPUState *d);   /* apu_vp.c */

/* Front-end stops waited out rather than played as silence: how many, how
 * many outlasted the wait, and the time spent. */
unsigned g_apu_trap_stalls, g_apu_trap_expired;
int64_t  g_apu_trap_stall_us;

static void *mcpx_apu_frame_thread(void *arg)
{
    MCPXAPUState *d = MCPX_APU_DEVICE(arg);
#if defined(__APPLE__)
    /* Guest threads run at user-interactive QoS on Darwin (win32_compat.c, so the title's
     * audio writer gets the guest CPU at scene loads). This thread produces every sample
     * the device plays: left at the default it lost the cores to them in a four-fighter
     * fight and the device ran dry 23 times in four minutes instead of 10. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    qemu_mutex_lock(&d->lock);

    while (!qatomic_read(&d->exiting)) {
        if (d->pause_requested && !g_test_tone.active && !g_mixer_active_count) {
            d->is_idle = true;
            qemu_cond_signal(&d->idle_cond);
            qemu_cond_wait(&d->cond, &d->lock);
            d->is_idle = false;
            continue;
        }

        /* Always run the audio output loop — the software mixer and test tone
         * need continuous frame delivery regardless of APU register state.
         * The VP/DSP pipeline (se_frame) only runs when registers allow it. */
        throttle(d);

        /* The doorbell ack stands in for the GP DSP, which on hardware runs
         * whatever the front end is doing. Tying it to se_frame stopped it
         * whenever FECTL was trapped or halted, and DirectSound then waits
         * forever to post its next command: Burnout 3 stalls in
         * sub_002F805E polling the same doorbell it was acked on at init. */
        mcpx_apu_dsp_ack_poll(d);

        int xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                NV_PAPU_SECTL_XCNTMODE);
        uint32_t fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
        bool apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) &&
                          !(fectl & NV_PAPU_FECTL_FEMETHMODE_HALTED);

        {
            /* RECOMP_APU_TRACE: every five seconds, for the first minute,
             * whether frames run, whether voices finish, whether the title has
             * the interrupt enabled, and where every voice stands -- the links
             * between a started voice and a title that sees it play. */
            extern unsigned g_apu_notifies;
            static int64_t next_ms;
            static unsigned lines, frames;
            static int trace = -1;
            static unsigned limit = 12;
            int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
            if (trace < 0) {
                /* RECOMP_APU_TRACE=<n> for n reports instead of twelve. */
                const char *e = getenv("RECOMP_APU_TRACE");
                trace = e != NULL;
                if (e && atoi(e) > 1)
                    limit = (unsigned)atoi(e);
            }
            frames++;
            if (trace && lines < limit && now_ms >= next_ms) {
                if (next_ms)
                    fprintf(stderr, "[APU] status: frames %u active %d notifies %u"
                            " ISTS=%08X IEN=%08X FECTL=%08X SECTL=%08X\n",
                            frames, apu_active, g_apu_notifies,
                            d->regs[NV_PAPU_ISTS], d->regs[NV_PAPU_IEN],
                            fectl, d->regs[NV_PAPU_SECTL]);
                if (next_ms) {
                    lines++;
                    mcpx_apu_vp_dump_voices(d);
                }
                next_ms = now_ms + 5000;
            }
        }

        /* A front-end trap or a voice notification sets set_irq; the line
         * follows it here, as xemu's frame loop does. Nothing acted on it
         * before, so the APU never interrupted: the first voice that went
         * idle with DirectSound asking for a trap (SE2FE_IDLE_VOICE) left
         * the front end trapped and every later frame silent. */
        if (d->set_irq) {
            d->set_irq = false;
            update_irq(d);
        }

        /* A front end that is trapped or halted is waiting for the title to
         * service the interrupt just raised, which takes it a millisecond or
         * so. Stop the clock for that long instead of playing the slices as
         * silence. This thread runs a period's eight slices back to back and
         * then sleeps, so once the front end stopped at slice k the rest of
         * the period was silent: a hole of 32 to 224 samples cut into
         * whatever else was playing. A title that asks to be told when a
         * voice ends stops on every effect -- 232 holes in two minutes of a
         * Def Jam fight, heard as distortion on every loud note. The frames
         * are made up afterwards, throttle() permitting. A front end that
         * stays stopped past the wait plays silence as before, so the
         * software mixer keeps its frames, and is not waited for again until
         * it has run. */
        {
            static int gave_up;
            static unsigned stalls, expired;

            if (apu_active) {
                gave_up = 0;
            } else if (!gave_up && xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF &&
                       !g_test_tone.active) {
                int64_t start_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME), waited_us = 0;

                while ((fectl & NV_PAPU_FECTL_FEMETHMODE) && waited_us < 20000 &&
                       !qatomic_read(&d->exiting)) {
                    if (waited_us < 2000) {
                        qemu_mutex_unlock(&d->lock);
                        SwitchToThread();
                        qemu_mutex_lock(&d->lock);
                    } else {
                        qemu_cond_timedwait(&d->cond, &d->lock, 1);
                    }
                    if (d->set_irq) {
                        d->set_irq = false;
                        update_irq(d);
                    }
                    fectl = qatomic_read(&d->regs[NV_PAPU_FECTL]);
                    waited_us = qemu_clock_get_us(QEMU_CLOCK_REALTIME) - start_us;
                }
                if (fectl & NV_PAPU_FECTL_FEMETHMODE) {
                    expired++;
                    gave_up = 1;
                }
                g_apu_trap_stalls = ++stalls;
                g_apu_trap_expired = expired;
                g_apu_trap_stall_us += waited_us;
                xcntmode = GET_MASK(qatomic_read(&d->regs[NV_PAPU_SECTL]),
                                    NV_PAPU_SECTL_XCNTMODE);
                apu_active = (xcntmode != NV_PAPU_SECTL_XCNTMODE_OFF) &&
                             !(fectl & NV_PAPU_FECTL_FEMETHMODE);
            }
        }

        if (apu_active && !g_test_tone.active) {
            /* Full pipeline: VP voices → DSP → monitor → waveOut */
            se_frame(d);
        } else {
            /* Lightweight: just monitor frame (test tone + software mixer) */
            mcpx_apu_monitor_frame(d);
            d->ep_frame_div++;
        }

        /* What xemu's frame thread does after each frame: turn a pending
         * notification (set by the voice processor or a trapped method) into
         * the interrupt line. Nothing here ever did, so the line never rose
         * even before there was anything to deliver it to. */
        if (d->set_irq) {
            update_irq(d);
            d->set_irq = false;
        }
        /* The pinned kernel timer owns guest ISR delivery; this worker mixes audio. */

        /* Let the guest in once per frame.
         *
         * The thread holds d->lock for its whole loop and only drops it inside
         * throttle()'s wait. Once voices really play, processing can run
         * behind real time, throttle never waits, and the lock is never
         * released -- while every VOICE_ON/OFF/RELEASE the title writes needs
         * it (voice_lock). A critical section is not fair, so the title's
         * thread starved there indefinitely: Burnout 3 froze on its vehicle
         * select, blocked in voice_lock at raised IRQL, which in turn held
         * off every USB interrupt. */
        qemu_mutex_unlock(&d->lock);
        SwitchToThread();
        qemu_mutex_lock(&d->lock);
    }

    qemu_mutex_unlock(&d->lock);
    return NULL;
}

/* ============================================================
 * Wait for idle / resume helpers
 * ============================================================ */

static void mcpx_apu_wait_for_idle(MCPXAPUState *d)
{
    d->pause_requested = true;
    qemu_cond_signal(&d->cond);
    while (!d->is_idle) {
        qemu_cond_wait(&d->idle_cond, &d->lock);
    }
}

static void mcpx_apu_resume(MCPXAPUState *d)
{
    d->pause_requested = false;
    qemu_cond_signal(&d->cond);
}

/* ============================================================
 * Reset
 * ============================================================ */

static void mcpx_apu_reset_locked(MCPXAPUState *d)
{
    memset(d->regs, 0, sizeof(d->regs));
    mcpx_apu_vp_reset(d);

    if (d->gp.dsp) {
        memset((void *)d->gp.dsp->core.pram_opcache, 0,
               sizeof(d->gp.dsp->core.pram_opcache));
    }
    if (d->ep.dsp) {
        memset((void *)d->ep.dsp->core.pram_opcache, 0,
               sizeof(d->ep.dsp->core.pram_opcache));
    }
    d->set_irq = false;
}

/* ============================================================
 * Public API: Init / Shutdown
 * ============================================================ */

MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(MCPXAPUState));
    if (!d) {
        fprintf(stderr, "[APU] Failed to allocate MCPXAPUState\n");
        return NULL;
    }

    g_apu_ram_ptr = ram_ptr;
    g_state = d;
    d->ram_ptr = ram_ptr;

    d->set_irq = false;
    d->exiting = false;
    d->is_idle = false;
    d->pause_requested = true;

    qemu_mutex_init(&d->lock);
    qemu_mutex_lock(&d->lock);
    qemu_cond_init(&d->cond);
    qemu_cond_init(&d->idle_cond);

    /* Init VP (voice processor) */
    mcpx_apu_vp_init(d);

    /* Init DSP (GP/EP - stubbed) */
    mcpx_apu_dsp_init(d);

    /* Init software mixer for DirectSound bridge */
    mixer_init();

    /* Init monitor (waveOut output) */
    Error *local_err = NULL;
    mcpx_apu_monitor_init(d, &local_err);
    if (local_err) {
        warn_reportf_err(local_err, "mcpx_apu_monitor_init failed: ");
    }

    /* Start background frame thread */
    qemu_thread_create(&d->apu_thread, "mcpx.apu_thread",
                       mcpx_apu_frame_thread, d, QEMU_THREAD_JOINABLE);
    mcpx_apu_wait_for_idle(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU] MCPX APU initialized (standalone)\n");
    fprintf(stderr, "[APU]   RAM pointer: %p\n", (void *)ram_ptr);
    fprintf(stderr, "[APU]   MMIO base: 0xFE800000 (512KB)\n");
    fprintf(stderr, "[APU]   VP: %d max voices, %d samples/frame\n",
            MCPX_HW_MAX_VOICES, NUM_SAMPLES_PER_FRAME);
    return d;
}

void mcpx_apu_shutdown(MCPXAPUState *d)
{
    if (!d) return;

    fprintf(stderr, "[APU] Shutting down MCPX APU...\n");

    qemu_mutex_lock(&d->lock);
    mcpx_apu_wait_for_idle(d);
    qatomic_set(&d->exiting, true);
    qemu_cond_signal(&d->cond);
    qemu_mutex_unlock(&d->lock);

    qemu_thread_join(&d->apu_thread);
    mcpx_apu_vp_finalize(d);
    mcpx_apu_monitor_finalize(d);

    free(d);
    g_state = NULL;
    fprintf(stderr, "[APU] Shutdown complete\n");
}

/* ============================================================
 * VP MMIO handlers (sub-region at +0x20000)
 *
 * These are called when the game writes to the VP PIO registers
 * to configure voices, SSL, etc.
 * ============================================================ */

uint64_t mcpx_apu_vp_read(void *opaque, hwaddr addr, unsigned int size);
void mcpx_apu_vp_write(void *opaque, hwaddr addr, uint64_t val, unsigned int size);

/* Dispatch a VP-region access (offset 0x20000-0x2FFFF from APU base) */
void mcpx_apu_dispatch_mmio(MCPXAPUState *d, hwaddr addr, uint64_t val,
                             unsigned int size, bool is_write)
{
    if (addr >= 0x20000 && addr < 0x30000) {
        /* VP region */
        hwaddr vp_addr = addr - 0x20000;
        if (is_write) {
            mcpx_apu_vp_write(d, vp_addr, val, size);
        }
        /* VP reads handled by caller if needed */
    } else if (addr < 0x20000) {
        /* Main APU registers */
        if (is_write) {
            mcpx_apu_write(d, addr, val, size);
        }
    }
    /* GP (0x30000) and EP (0x50000) regions ignored for now */
}

/* ============================================================
 * Public MMIO API (called from VEH or MMIO hook)
 * addr is offset from APU base (0xFE800000)
 * ============================================================ */

uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size)
{
    if (!d) return 0;
    if (addr >= 0x20000 && addr < 0x30000) {
        return mcpx_apu_vp_read(d, addr - 0x20000, size);
    } else if (addr < 0x20000) {
        return mcpx_apu_read(d, (hwaddr)addr, size);
    }
    return 0;
}

void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val, unsigned int size)
{
    if (!d) return;
    mcpx_apu_dispatch_mmio(d, (hwaddr)addr, val, size, true);
}

/* ============================================================
 * APU Test Tone - Direct waveOut sine generator
 *
 * Bypasses the VP pipeline entirely and writes a 440Hz sine wave
 * directly to the monitor frame_buf. This verifies that waveOut
 * output works correctly.
 * ============================================================ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void mcpx_apu_play_test_tone(MCPXAPUState *d)
{
    if (!d) {
        fprintf(stderr, "[APU-TEST] No APU state\n");
        return;
    }

    if (g_test_tone.active) {
        /* Toggle off */
        g_test_tone.active = false;
        fprintf(stderr, "[APU-TEST] Test tone OFF\n");
        return;
    }

    /* 440Hz at 48kHz sample rate */
    g_test_tone.phase = 0.0;
    g_test_tone.phase_inc = 2.0 * M_PI * 440.0 / 48000.0;
    g_test_tone.amplitude = 6000;  /* ~18% of full scale */
    g_test_tone.active = true;

    /* Make sure waveOut is running - enable SECTL and resume APU thread */
    qemu_mutex_lock(&d->lock);
    d->regs[NV_PAPU_SECTL] = NV_PAPU_SECTL_XCNTMODE & ~NV_PAPU_SECTL_XCNTMODE_OFF;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_FREE_RUNNING;
    /* Initialize empty voice lists so VP frame doesn't crash */
    d->regs[NV_PAPU_TVL2D] = 0xFFFF;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    mcpx_apu_resume(d);
    qemu_mutex_unlock(&d->lock);

    fprintf(stderr, "[APU-TEST] Test tone ON - 440Hz sine, amplitude=%d\n",
            g_test_tone.amplitude);
}

/* ============================================================
 * Software mixer - mixes DirectSound buffers to waveOut
 *
 * This bypasses the VP hardware voice pipeline entirely.
 * DirectSound buffers register PCM data here, and the APU
 * frame thread mixes them into the monitor frame_buf.
 * ============================================================ */

static void mixer_init(void)
{
    if (g_mixer_initialized) return;
    InitializeCriticalSection(&g_mixer_cs);
    memset(g_mixer_voices, 0, sizeof(g_mixer_voices));
    g_mixer_initialized = true;
}

int apu_mixer_alloc_voice(void)
{
    if (!g_mixer_initialized) mixer_init();
    EnterCriticalSection(&g_mixer_cs);
    for (int i = 0; i < APU_MIXER_MAX_VOICES; i++) {
        if (!g_mixer_voices[i].active && !g_mixer_voices[i].pcm_data) {
            g_mixer_voices[i].volume = 1.0f;
            g_mixer_voices[i].sample_rate = 44100;
            g_mixer_voices[i].num_channels = 2;
            LeaveCriticalSection(&g_mixer_cs);
            return i;
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
    return -1;
}

void apu_mixer_free_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    apu_mixer_stop(slot);
    g_mixer_voices[slot].pcm_data = NULL;
    g_mixer_voices[slot].pcm_bytes = 0;
    g_mixer_voices[slot].play_offset = 0;
    LeaveCriticalSection(&g_mixer_cs);
}

APUMixerVoice *apu_mixer_get_voice(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return NULL;
    return &g_mixer_voices[slot];
}

int apu_mixer_set_position(int slot, uint32_t byte_offset)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return 0;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    uint32_t frame_bytes = v->num_channels * sizeof(int16_t);
    int valid = (v->num_channels == 1 || v->num_channels == 2) &&
                byte_offset < v->pcm_bytes &&
                byte_offset / frame_bytes < v->pcm_bytes / frame_bytes;
    if (valid) v->play_offset = ((uint64_t)(byte_offset / frame_bytes)) << 16;
    LeaveCriticalSection(&g_mixer_cs);
    return valid;
}

void apu_mixer_get_state(int slot, uint32_t *byte_offset, int *active, int *looping)
{
    if (byte_offset) *byte_offset = 0;
    if (active) *active = 0;
    if (looping) *looping = 0;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (byte_offset) *byte_offset = (uint32_t)(v->play_offset >> 16) *
                                    v->num_channels * sizeof(int16_t);
    if (active) *active = v->active;
    if (looping) *looping = v->active && v->looping;
    LeaveCriticalSection(&g_mixer_cs);
}

void apu_mixer_play(int slot, int looping)
{
    if (g_audio_muted) return;
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    APUMixerVoice *v = &g_mixer_voices[slot];
    if (!v->pcm_data || (v->num_channels != 1 && v->num_channels != 2) ||
        v->pcm_bytes < v->num_channels * sizeof(int16_t)) {
        LeaveCriticalSection(&g_mixer_cs);
        return;
    }
    v->looping = looping;
    if (!v->active) InterlockedIncrement((volatile LONG *)&g_mixer_active_count);
    v->active = 1;

    static int play_log_count = 0;
    if (play_log_count < 20) {
        fprintf(stderr, "[APU-MIX] Play voice %d: %u bytes, %u ch, %u Hz, vol=%.2f, loop=%d\n",
                slot, v->pcm_bytes, v->num_channels, v->sample_rate, v->volume, looping);
        play_log_count++;
    }
    LeaveCriticalSection(&g_mixer_cs);

    /* The frame thread takes the APU lock before the mixer lock. */
    extern MCPXAPUState *g_state;
    if (g_state) {
        qemu_mutex_lock(&g_state->lock);
        g_state->pause_requested = false;
        qemu_cond_signal(&g_state->cond);
        qemu_mutex_unlock(&g_state->lock);
    }
}

void apu_mixer_stop(int slot)
{
    if (slot < 0 || slot >= APU_MIXER_MAX_VOICES) return;
    EnterCriticalSection(&g_mixer_cs);
    if (g_mixer_voices[slot].active) {
        g_mixer_voices[slot].active = 0;
        InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
    }
    LeaveCriticalSection(&g_mixer_cs);
}

/* Mix all active voices into frame_buf. Called from mcpx_apu_monitor_frame.
 * Keep 16 fractional bits in a wide offset so buffers can exceed 65536 frames. */
static void mixer_render(int16_t frame_buf[][2], int num_samples)
{
    if (!g_mixer_initialized) return;
    EnterCriticalSection(&g_mixer_cs);

    for (int v = 0; v < APU_MIXER_MAX_VOICES; v++) {
        APUMixerVoice *voice = &g_mixer_voices[v];
        if (!voice->active || !voice->pcm_data || voice->pcm_bytes == 0)
            continue;

        uint32_t total_frames = voice->pcm_bytes / sizeof(int16_t);
        if (voice->num_channels == 2) total_frames /= 2;
        if (total_frames == 0) continue;

        /* Fixed-point 16.16 increment per output sample */
        uint64_t inc = ((uint64_t)voice->sample_rate << 16) / 48000;
        uint64_t pos = voice->play_offset;
        uint64_t end = (uint64_t)total_frames << 16;
        float vol = voice->volume;

        for (int i = 0; i < num_samples; i++) {
            uint32_t src_frame = pos >> 16;

            if (src_frame >= total_frames) {
                if (voice->looping) {
                    pos %= end;
                    src_frame = (uint32_t)(pos >> 16);
                } else {
                    pos = 0;
                    voice->active = 0;
                    InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
                    break;
                }
            }

            int32_t left, right;
            if (voice->num_channels >= 2) {
                left  = (int32_t)(voice->pcm_data[src_frame * 2] * vol);
                right = (int32_t)(voice->pcm_data[src_frame * 2 + 1] * vol);
            } else {
                left = right = (int32_t)(voice->pcm_data[src_frame] * vol);
            }

            /* Accumulate (mix) into frame_buf with clamping */
            int32_t mixed_l = frame_buf[i][0] + left;
            int32_t mixed_r = frame_buf[i][1] + right;
            if (mixed_l > 32767) mixed_l = 32767;
            if (mixed_l < -32768) mixed_l = -32768;
            if (mixed_r > 32767) mixed_r = 32767;
            if (mixed_r < -32768) mixed_r = -32768;
            frame_buf[i][0] = (int16_t)mixed_l;
            frame_buf[i][1] = (int16_t)mixed_r;

            pos += inc;
        }

        voice->play_offset = pos;
        uint32_t end_frame = pos >> 16;
        if (end_frame >= total_frames) {
            if (voice->looping) {
                voice->play_offset = pos % end;
            } else if (voice->active) {
                voice->play_offset = 0;
                voice->active = 0;
                InterlockedDecrement((volatile LONG *)&g_mixer_active_count);
            }
        }
    }
    LeaveCriticalSection(&g_mixer_cs);
}
