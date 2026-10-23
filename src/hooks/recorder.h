/* The play-test flight recorder. See recorder.c and docs/playtest-recorder.md.
 *
 * Kept free of the runtime's headers: everything it measures is handed to it by
 * the caller (d3d11_translator.c for frames and pictures, recomp_manual.c and the
 * telemetry for game events), so tests/unit/test_recorder_native.py builds it on
 * its own with an ordinary C compiler. */

#ifndef DEFJAM_RECORDER_H
#define DEFJAM_RECORDER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecorderConfig {
    int enabled;
    const char *root;           /* sessions are made under <root>/sessions ("logs") */
    int video_fps;              /* pictures a second kept in the ring; 0 keeps none */
    int ring_seconds;           /* how far back a bundle reaches */
    int thumb_height;           /* the ring's pictures, this many rows (width follows the picture's shape) */
    int after_seconds;          /* a marked bundle also holds this long after the mark */
    int hitch_ms;               /* a frame at least this long, and 2.5 times the recent median, is a hitch */
    int auto_bundle_hitch_ms;   /* a hitch at least this long makes a bundle by itself; 0: never */
    int hang_seconds;           /* no frame for this long makes a "hang" bundle; 0: never */
    int keep_sessions;          /* older sessions keep their bundles and events, and lose the rest */
    int guest_ram;              /* crash and hang bundles include guest memory */
    const void *(*guest_base)(void);   /* guest address 0 in host memory, asked when a bundle is written
                                          (guest memory is set up after the recorder starts) */
    size_t guest_size;
    const char *log_path;       /* the file stderr goes to, when known */
    const char *audio_path;     /* the mixed audio being written (raw 48 kHz stereo 16-bit), when known */
    const char *build;          /* version and source identity, for bundle.json */
    const char *settings_json;  /* a JSON object: the settings this run started with */
    int mark_vk;                /* the key that marks a moment (a Windows virtual-key code) */
    const char *mark_key_name;  /* its name for the log ("F9"), read by recorder_init only; NULL: the code */
    /* The hitch probe: when no frame has been presented for stall_probe_ms (0: never), a
     * thread of the recorder's calls stall_probe, again every 50 ms up to six times a frame.
     * It writes two JSON objects, the thread holding the guest CPU and every thread's stack;
     * the first goes into the frame's hitch event, all of them into stalls.jsonl. */
    int stall_probe_ms;
    int (*stall_probe)(char *holder, size_t holder_size, char *all, size_t all_size);
} RecorderConfig;

/* What the caller measured for one frame. Counters are the runtime's own and
 * may be cumulative or reset; the recorder takes differences. */
typedef struct RecorderFrameIn {
    double present_ms;          /* time spent in Present (waiting for the display included) */
    double capture_ms;          /* time the recorder's own picture read-back took, 0 when none */
    uint32_t draws, vertices, clears;   /* the translator's running totals */
    int audio_dropped, audio_dry;       /* the audio device's counters */
    long long step;             /* the game's simulation step in a fight, -1 outside one */
    long long log_bytes;        /* how much of the log has been written, -1 unknown */
    /* The translator's texture cache: uploads (new and changed in place), their bytes and
     * evictions as running totals; entries and bytes held now. hd_cache_bytes: the HD
     * texture pack's resident replacements now, 0 without a pack. */
    uint32_t tex_uploads, tex_evictions;
    unsigned long long tex_upload_bytes;
    uint32_t tex_cached;
    unsigned long long tex_cache_bytes, hd_cache_bytes;
} RecorderFrameIn;

/* Make a session folder, start the worker. Returns 1 when recording. */
int  recorder_init(const RecorderConfig *cfg);
int  recorder_on(void);
const char *recorder_session_dir(void);
/* The mixed-audio file being written, once it is known (pc_debug.c points
 * RECOMP_APU_PCM into the session after the folder exists). */
void recorder_set_audio_path(const char *path);
/* The recorder's clock, in milliseconds (monotonic). */
double recorder_now_ms(void);

/* The presenting thread, once a frame: begin before reading the picture and
 * presenting it, end after. */
void recorder_frame_begin(void);
void recorder_frame_end(const RecorderFrameIn *in);

/* Whether to hand the recorder this frame's picture: 1 for the ring, 2 when a
 * full-size still is also wanted. */
int  recorder_picture_wanted(void);
/* The picture: rows of R, G, B, A bytes. `gamma` (or NULL) is applied, as the
 * window applies it. */
void recorder_picture(const uint8_t *rgba, int pitch, int width, int height,
                      const uint8_t gamma[3][256]);
/* The same when the picture is read back asynchronously and arrives a frame or two
 * late. After recorder_picture_wanted() said yes, take this frame's request (0: none);
 * when the picture comes, hand it over with that tag and it is filed under the frame and
 * time of the request. `bgra`: the bytes run B, G, R, A. */
uint64_t recorder_picture_request(void);
void recorder_picture_tagged(uint64_t tag, const uint8_t *pixels, int pitch, int width, int height,
                             int bgra, const uint8_t gamma[3][256]);

/* One line of events.jsonl. `fields` is the inside of a JSON object
 * ("\"a\":1,\"b\":\"x\"") or NULL. Any thread. */
void recorder_event(const char *kind, const char *fields);
/* The same with one string field, escaped here. */
void recorder_event_text(const char *kind, const char *key, const char *text);

/* Ask for a bundle of the last seconds. Any thread; the worker writes it. */
void recorder_mark(const char *reason);
/* Write a bundle now, from a crashing thread. Best effort, once. */
void recorder_on_crash(const char *what);
/* The same from the Windows exception handler, with its EXCEPTION_POINTERS for the
 * minidump (NULL elsewhere). */
void recorder_on_crash_os(const char *what, void *os_exception);
void recorder_shutdown(void);

int  recorder_mark_vk(void);
/* The game's view mode changed (src/patches/widescreen.c): 1 is a Story cutscene. */
void recorder_set_view_mode(int mode);
/* A one-line account for the overlay: frames, hitches, bundles, the folder. */
void recorder_status(char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif
