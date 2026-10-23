/**
 * XAudio2 Audio Output Backend
 *
 * Provides low-latency audio output via XAudio2 (Win7+).
 * Called from the APU monitor frame to submit mixed samples.
 * Falls back gracefully if XAudio2 is unavailable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_xaudio2.h"
#if defined(__ANDROID__)
#include "platform/android_thread_policy.h"
#endif

/* The XAudio2 backend is Windows-only. On Linux all xa2_* functions are
 * stubbed to report inactive; real audio output via SDL2 comes later. */
#if defined(_WIN32)

#define COBJMACROS
#include <windows.h>
#include <xaudio2.h>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3

static IXAudio2               *g_xa2 = NULL;
static IXAudio2MasteringVoice *g_xa2_master = NULL;
static IXAudio2SourceVoice    *g_xa2_source = NULL;
static int16_t                 g_xa2_bufs[XA2_NUM_BUFS][XA2_BUF_SAMPLES][2];
static int                     g_xa2_next_buf = 0;
static int                     g_xa2_initialized = 0;
static int                     g_xa2_frames_written = 0;
/* Submissions refused because the queue was full, and submissions that
 * found it empty (the device had run dry: a gap in what is heard). */
int                            g_xa2_dropped, g_xa2_starved;

int xa2_init(void)
{
    HRESULT hr;
    int com_initialized;
    WAVEFORMATEX wfx = { 0 };

    if (g_xa2_initialized) return 1;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        fprintf(stderr, "[XA2] CoInitializeEx failed: 0x%08lX\n", hr);
        return 0;
    }
    com_initialized = SUCCEEDED(hr);

    hr = XAudio2Create(&g_xa2, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !g_xa2) {
        fprintf(stderr, "[XA2] XAudio2Create failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2_CreateMasteringVoice(g_xa2, &g_xa2_master,
        XA2_CHANNELS, XA2_SAMPLE_RATE, 0, NULL, NULL, 0);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateMasteringVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = XA2_CHANNELS;
    wfx.nSamplesPerSec  = XA2_SAMPLE_RATE;
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = XA2_CHANNELS * 2;
    wfx.nAvgBytesPerSec = XA2_SAMPLE_RATE * wfx.nBlockAlign;

    hr = IXAudio2_CreateSourceVoice(g_xa2, &g_xa2_source,
        &wfx, 0, XAUDIO2_DEFAULT_FREQ_RATIO, NULL, NULL, NULL);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] CreateSourceVoice failed: 0x%08lX\n", hr);
        goto fail;
    }

    hr = IXAudio2SourceVoice_Start(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
    if (FAILED(hr)) {
        fprintf(stderr, "[XA2] Start failed: 0x%08lX\n", hr);
        goto fail;
    }

    g_xa2_next_buf = 0;
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;

    fprintf(stderr, "[XA2] XAudio2 initialized (%d Hz stereo 16-bit, %d x %d-sample buffers)\n",
            XA2_SAMPLE_RATE, XA2_NUM_BUFS, XA2_BUF_SAMPLES);
    return 1;

fail:
    xa2_shutdown();
    /* Failed initialization still runs on the COM-initializing thread. */
    if (com_initialized) CoUninitialize();
    return 0;
}

void xa2_shutdown(void)
{
    if (g_xa2_source) {
        IXAudio2SourceVoice_Stop(g_xa2_source, 0, XAUDIO2_COMMIT_NOW);
        IXAudio2SourceVoice_FlushSourceBuffers(g_xa2_source);
        g_xa2_source->lpVtbl->DestroyVoice(g_xa2_source);
        g_xa2_source = NULL;
    }
    if (g_xa2_master) {
        g_xa2_master->lpVtbl->DestroyVoice(g_xa2_master);
        g_xa2_master = NULL;
    }
    if (g_xa2) {
        IXAudio2_Release(g_xa2);
        g_xa2 = NULL;
    }

    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void)
{
    return g_xa2_initialized;
}

/* Submit a buffer of mixed samples to XAudio2.
 * Called from APU frame thread. Returns 1 if buffer was submitted. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    XAUDIO2_VOICE_STATE state;
    XAUDIO2_BUFFER xbuf;
    int idx;
    int copy_samples;
    HRESULT hr;

    if (!g_xa2_initialized || !g_xa2_source) return 0;

    IXAudio2SourceVoice_GetState(g_xa2_source, &state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    if (state.BuffersQueued == 0 && g_xa2_frames_written > 0)
        g_xa2_starved++;
    if ((int)state.BuffersQueued >= XA2_NUM_BUFS) {
        g_xa2_dropped++;
        return 0;
    }

    idx = g_xa2_next_buf;
    copy_samples = (num_samples > XA2_BUF_SAMPLES) ? XA2_BUF_SAMPLES : num_samples;
    memcpy(g_xa2_bufs[idx], samples, copy_samples * XA2_CHANNELS * sizeof(int16_t));

    memset(&xbuf, 0, sizeof(xbuf));
    xbuf.AudioBytes = copy_samples * XA2_CHANNELS * sizeof(int16_t);
    xbuf.pAudioData = (const BYTE *)g_xa2_bufs[idx];

    hr = IXAudio2SourceVoice_SubmitSourceBuffer(g_xa2_source, &xbuf, NULL);
    if (FAILED(hr)) return 0;

    g_xa2_next_buf = (idx + 1) % XA2_NUM_BUFS;
    g_xa2_frames_written++;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return XA2_BUF_SAMPLES;
}

#else /* !_WIN32 -- the same interface on an SDL audio device */

#ifdef XBOXRECOMP_HAVE_SDL3
#include <SDL3/SDL.h>
#else
#include <SDL.h>
#endif
#include <stdlib.h>
#include <string.h>

#define XA2_SAMPLE_RATE   48000
#define XA2_CHANNELS      2
#define XA2_BUF_SAMPLES   1024   /* ~21ms per submission */
#define XA2_NUM_BUFS      3
#define RING_FRAMES       8192   /* power of two, well over what is ever queued */
/* The device starts, and starts again after running out, once this much is
 * waiting: two submissions, 43 ms. Without it the ring sits near empty and
 * any lateness in the mixer is a gap. Above XA2_RING_MAX a submission is
 * refused, which bounds the delay when the mixer's clock runs fast. */
#define XA2_PREROLL       (2 * XA2_BUF_SAMPLES)
#define XA2_RING_MAX      (4 * XA2_BUF_SAMPLES)

/* A ring the device's callback drains. SDL's own queue cannot tell "empty
 * because the device took it all into its buffer" from "empty because nothing
 * was sent", and the second is the one that is heard. */
static SDL_AudioDeviceID g_sdl_dev;
static int16_t  g_ring[RING_FRAMES][XA2_CHANNELS];
static unsigned g_ring_r, g_ring_w;         /* frame counters, free-running */
static int g_xa2_initialized;
static int g_xa2_frames_written;
static int g_dry;                           /* the callback is in a gap */
static int g_primed;                        /* enough is queued to play from */
/* As on Windows: submissions refused because three buffers were waiting, and
 * times the device ran out (a gap in what is heard). */
int  g_xa2_dropped, g_xa2_starved;

static void sdl_callback(void *user, Uint8 *stream, int len)
{
#if defined(__ANDROID__)
    static _Thread_local int named;
    if (!named) {
        defjam_android_native_thread_enter("audio", "sdl_callback", (uintptr_t)sdl_callback);
        named = 1;
    }
#endif
    int16_t (*out)[XA2_CHANNELS] = (void *)stream;
    unsigned want = (unsigned)len / (XA2_CHANNELS * sizeof(int16_t)), i;
    unsigned have = g_ring_w - g_ring_r;

    (void)user;
    if (!g_primed) {
        if (have < XA2_PREROLL) {
            memset(stream, 0, (size_t)len);
            return;
        }
        g_primed = 1;
    }
    if (have > want)
        have = want;
    for (i = 0; i < have; i++)
        memcpy(out[i], g_ring[(g_ring_r + i) % RING_FRAMES], sizeof out[i]);
    g_ring_r += have;
    if (have < want) {
        memset(out[have], 0, (size_t)(want - have) * sizeof out[0]);
        /* One gap however many callbacks it lasts, and none before the title
         * has sent anything. */
        if (!g_dry && g_xa2_frames_written > 0) {
            g_dry = 1;
            g_xa2_starved++;
        }
        g_primed = 0;
    } else {
        g_dry = 0;
    }
}

#ifdef XBOXRECOMP_HAVE_SDL3
/* SDL3 asks a stream for more instead of handing over a buffer. */
static void SDLCALL sdl3_feed(void *user, SDL_AudioStream *stream, int additional, int total)
{
    static Uint8 chunk[4096];
    (void)total;
    while (additional > 0) {
        int n = additional < (int)sizeof chunk ? additional : (int)sizeof chunk;
        n -= n % (XA2_CHANNELS * (int)sizeof(int16_t));
        if (n <= 0)
            break;
        sdl_callback(user, chunk, n);
        SDL_PutAudioStreamData(stream, chunk, n);
        additional -= n;
    }
}
static SDL_AudioStream *g_sdl_stream;
#endif

int xa2_init(void)
{
#ifdef XBOXRECOMP_HAVE_SDL3
    SDL_AudioSpec have;
#else
    SDL_AudioSpec want, have;
#endif

    if (g_xa2_initialized)
        return 1;
    /* A run without a display is an unattended one: keep its timing (the
     * dummy driver consumes at the real rate) and keep it quiet. */
    if (getenv("RECOMP_HEADLESS") && !getenv("SDL_AUDIODRIVER") && !getenv("SDL_AUDIO_DRIVER")) {
#ifdef XBOXRECOMP_HAVE_SDL3
        /* SDL3 reads its own copy of the environment, taken before this. */
        SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
#else
        setenv("SDL_AUDIODRIVER", "dummy", 1);
#endif
    }
#ifdef XBOXRECOMP_HAVE_SDL3
    {
        SDL_AudioSpec spec;
        if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[XA2] SDL audio unavailable: %s\n", SDL_GetError());
            return 0;
        }
        spec.freq = XA2_SAMPLE_RATE;
        spec.format = SDL_AUDIO_S16;
        spec.channels = XA2_CHANNELS;
        g_sdl_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec,
                                                 sdl3_feed, NULL);
        if (!g_sdl_stream) {
            fprintf(stderr, "[XA2] no audio device: %s\n", SDL_GetError());
            return 0;
        }
        g_sdl_dev = SDL_GetAudioStreamDevice(g_sdl_stream);
        SDL_ResumeAudioStreamDevice(g_sdl_stream);
        have.freq = spec.freq;
        have.channels = spec.channels;
    }
#else
    if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "[XA2] SDL audio unavailable: %s\n", SDL_GetError());
        return 0;
    }
    SDL_zero(want);
    want.freq = XA2_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = XA2_CHANNELS;
    want.samples = 512;
    want.callback = sdl_callback;
    g_sdl_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_sdl_dev) {
        fprintf(stderr, "[XA2] no audio device: %s\n", SDL_GetError());
        return 0;
    }
    SDL_PauseAudioDevice(g_sdl_dev, 0);
#endif
    g_xa2_initialized = 1;
    g_xa2_frames_written = 0;
    fprintf(stderr, "[XA2] SDL audio output initialized (%d Hz, %d channels, driver %s)\n",
            have.freq, have.channels, SDL_GetCurrentAudioDriver());
    return 1;
}

void xa2_shutdown(void)
{
#ifdef XBOXRECOMP_HAVE_SDL3
    if (g_sdl_stream) {
        SDL_DestroyAudioStream(g_sdl_stream);
        g_sdl_stream = NULL;
    }
    g_sdl_dev = 0;
#else
    if (g_sdl_dev) {
        SDL_CloseAudioDevice(g_sdl_dev);
        g_sdl_dev = 0;
    }
#endif
    if (g_xa2_initialized)
        fprintf(stderr, "[XA2] Shut down (%d frames written)\n", g_xa2_frames_written);
    g_xa2_initialized = 0;
}

int xa2_is_active(void) { return g_xa2_initialized; }

int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    unsigned n = (unsigned)(num_samples > XA2_BUF_SAMPLES ? XA2_BUF_SAMPLES : num_samples), i;
    int ok = 1;

    if (!g_xa2_initialized || !g_sdl_dev)
        return 0;
    /* A gap in the sound goes in the log, from here rather than from the device's own
     * thread: when it happened and how many so far, so a stutter can be lined up with
     * what the game was doing. */
    {
        static int told, seen;
        if (g_xa2_starved != seen && told < 60) {
            fprintf(stderr, "[XA2] the device ran dry (gap %d, %d frames written, %d submissions refused)\n",
                    g_xa2_starved, g_xa2_frames_written, g_xa2_dropped);
            told++;
        }
        seen = g_xa2_starved;
    }
#ifdef XBOXRECOMP_HAVE_SDL3
    SDL_LockAudioStream(g_sdl_stream);
#else
    SDL_LockAudioDevice(g_sdl_dev);
#endif
    if (g_ring_w - g_ring_r + n > XA2_RING_MAX) {
        g_xa2_dropped++;
        ok = 0;
    } else {
        for (i = 0; i < n; i++)
            memcpy(g_ring[(g_ring_w + i) % RING_FRAMES], samples + i * XA2_CHANNELS,
                   sizeof g_ring[0]);
        g_ring_w += n;
        g_xa2_frames_written++;
    }
#ifdef XBOXRECOMP_HAVE_SDL3
    SDL_UnlockAudioStream(g_sdl_stream);
#else
    SDL_UnlockAudioDevice(g_sdl_dev);
#endif
    return ok;
}

int xa2_get_buffer_size(void) { return XA2_BUF_SAMPLES; }

#endif /* _WIN32 */
