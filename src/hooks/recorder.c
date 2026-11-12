/*
 * The play-test flight recorder.
 *
 * A play-test used to leave a log with no clock in it, and nothing else. A
 * hitch, a picture that was wrong for a second or a pop in the sound could be
 * described afterwards but not located. With [debug] record = true (or
 * RECOMP_RECORD=1) every run keeps, under logs/sessions/<start time>/:
 *
 *   frames.csv     one row per presented frame: when it ended, how long it took,
 *                  how much of that was Present, the game, and this recorder's own
 *                  read-back, the draw counts, the audio device's dropped and dry
 *                  counts, the game's fight step, and how much of the log and of
 *                  the audio capture had been written by then. The last two put
 *                  every log line and every audio sample on the same clock as the
 *                  frames without changing the log's format (scripts/triage.py and
 *                  the harness parse it as it is).
 *   events.jsonl   what happened, when: front-end calls (the screen in use), hitches,
 *                  audio drop-outs, cutscene views, match events, marks, bundles,
 *                  memory every five seconds.
 *   audio.pcm      the mixed output, through the runtime's RECOMP_APU_PCM.
 *   session.json   build, platform and settings.
 *
 * and in memory the last seconds of pictures (a few a second, a quarter of the
 * console's size). F9, the overlay's Mark button, a crash, a hang, or a long
 * hitch (when asked) writes a *bundle*: those pictures, a full-size still, the
 * rows and events of the same seconds, the end of the log and of the audio.
 * scripts/bug-bundle.py turns a bundle into what an agent can read.
 *
 * The picture read-back is synchronous on both renderers, so it costs the frame
 * it is taken on; frames.csv says how much (capture_ms), and the analysis
 * subtracts it before calling anything a hitch.
 *
 * Kept free of the runtime's headers (see recorder.h): its own small platform
 * layer below, so tests/unit/test_recorder_native.py builds it with a plain C
 * compiler.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recorder.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- platform -------------------------------------------------------------------------------- */

#if defined(_WIN32)
#include <windows.h>
#include <direct.h>
#include <errno.h>
#include <sys/stat.h>
#include <psapi.h>

typedef CRITICAL_SECTION RecMutex;
static void mtx_init(RecMutex *m) { InitializeCriticalSection(m); }
static void mtx_lock(RecMutex *m) { EnterCriticalSection(m); }
static int  mtx_trylock(RecMutex *m) { return TryEnterCriticalSection(m) ? 1 : 0; }
static void mtx_unlock(RecMutex *m) { LeaveCriticalSection(m); }
static void sleep_ms(int ms) { Sleep((DWORD)ms); }
static double now_ms(void)
{
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}
static int make_dir(const char *p) { return _mkdir(p) == 0 || errno == EEXIST; }
static long long file_size(const char *p)
{
    struct __stat64 st;
    return _stat64(p, &st) == 0 ? (long long)st.st_size : -1;
}
static DWORD WINAPI worker_entry(LPVOID arg);
static void start_worker(void) { CreateThread(NULL, 0, worker_entry, NULL, 0, NULL); }
static DWORD WINAPI prober_entry(LPVOID arg);
static void start_prober(void) { CreateThread(NULL, 0, prober_entry, NULL, 0, NULL); }
static long long private_bytes(void)
{
    PROCESS_MEMORY_COUNTERS_EX pmc;
    memset(&pmc, 0, sizeof pmc);
    pmc.cb = sizeof pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc))
        return (long long)pmc.PrivateUsage;
    return -1;
}
static int process_id(void) { return (int)GetCurrentProcessId(); }
/* Names of the entries of `dir`, through `each`. */
static void list_dir(const char *dir, void (*each)(const char *name, void *user), void *user)
{
    char pattern[700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pattern, sizeof pattern, "%s\\*", dir);
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] != '.') each(fd.cFileName, user);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#define SEP "\\"
#else
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

typedef pthread_mutex_t RecMutex;
static void mtx_init(RecMutex *m)
{
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
}
static void mtx_lock(RecMutex *m) { pthread_mutex_lock(m); }
static int  mtx_trylock(RecMutex *m) { return pthread_mutex_trylock(m) == 0; }
static void mtx_unlock(RecMutex *m) { pthread_mutex_unlock(m); }
static void sleep_ms(int ms) { usleep((useconds_t)ms * 1000u); }
static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
static int make_dir(const char *p) { return mkdir(p, 0755) == 0 || errno == EEXIST; }
static long long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}
static void *worker_entry(void *arg);
static void start_worker(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, worker_entry, NULL) == 0) pthread_detach(t);
}
static void *prober_entry(void *arg);
static void start_prober(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, prober_entry, NULL) == 0) pthread_detach(t);
}
static long long private_bytes(void)
{
#if defined(__APPLE__)
    mach_task_basic_info_data_t info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        return (long long)info.resident_size;
    return -1;
#else
    long pages_total = 0, pages_resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f) return -1;
    if (fscanf(f, "%ld %ld", &pages_total, &pages_resident) != 2) pages_resident = -1;
    fclose(f);
    return pages_resident < 0 ? -1 : (long long)pages_resident * (long long)sysconf(_SC_PAGESIZE);
#endif
}
static int process_id(void) { return (int)getpid(); }
static void list_dir(const char *dir, void (*each)(const char *name, void *user), void *user)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') each(e->d_name, user);
    closedir(d);
}
#define SEP "/"
#endif

/* ---- state ------------------------------------------------------------------------------------ */

typedef struct FrameRec {
    unsigned long long frame;
    double t, interval, present, work, capture;
    uint32_t draws, vertices, clears;
    int dropped, dry;           /* this frame's increments */
    long long step, log_bytes, pcm_bytes;
    uint32_t tex_uploads, tex_evictions, tex_cached;   /* this frame's uploads and evictions; entries held */
    double tex_upload_kb, tex_cache_mb, hd_cache_mb;
} FrameRec;

typedef struct PicMeta { unsigned long long frame; double t; } PicMeta;

#define EVENT_RING 8192
#define RECENT 120
#define PATH_LEN 700

static RecorderConfig g_cfg;
static char g_root[PATH_LEN], g_session[PATH_LEN], g_log_path[PATH_LEN], g_audio_path[PATH_LEN];
static char g_build[256];
static char *g_settings_json;
static volatile int g_on, g_stop;
static double g_t0;

static RecMutex g_lock;             /* frame history, events, the files */
static RecMutex g_pic_lock;         /* the picture ring and the still */
static FILE *g_frames_file, *g_events_file;

static FrameRec *g_hist;
static size_t g_hist_cap;
static unsigned long long g_hist_n;   /* rows ever written; the ring holds the last g_hist_cap */

static char *g_ev[EVENT_RING];
static double g_ev_t[EVENT_RING];
static unsigned long long g_ev_n;

static uint8_t *g_ring;
static PicMeta *g_ring_meta;
static int g_ring_slots, g_tw, g_th;
static size_t g_slot_bytes;
static unsigned long long g_ring_n;
static double g_next_pic;
static volatile int g_frozen;
static int g_pic_mask;

static uint8_t *g_still;
static int g_still_w, g_still_h;
static volatile int g_still_wanted, g_still_ready;
static double g_still_t;
static unsigned long long g_still_frame;

static unsigned long long g_frame;
static double g_begin, g_last_end;
/* The hitch probe's findings for the frame being waited for (under g_lock). */
#define PROBE_HOLDER 6144
static FILE *g_stalls_file;
static unsigned long long g_probe_frame = ~0ull;
static int g_probe_n;
static double g_probe_since;
static char g_probe_first[PROBE_HOLDER], g_probe_last[PROBE_HOLDER];
static int g_last_dropped = -1, g_last_dry = -1;
static long long g_total_dropped, g_total_dry, g_hitches;
static double g_recent[RECENT];
static int g_recent_n, g_recent_i;
static double g_median = 16.67;

static volatile int g_mark_pending;
static char g_mark_reason[48];
static double g_mark_t;
static unsigned long long g_mark_frame;   /* the frame the mark was made in, not the one the bundle is written in */
static int g_bundles, g_auto_bundles;
static double g_last_auto;
static char g_last_bundle[PATH_LEN];
static volatile int g_crashed;

static char g_last_screen[256];
static char g_last_game[1024];
static int g_view_mode = -1;

/* ---- small helpers ------------------------------------------------------------------------------ */

static double rel_ms(double t) { return t - g_t0; }

static void copy_text(char *out, size_t size, const char *in)
{
    if (!size) return;
    snprintf(out, size, "%s", in ? in : "");
}

/* JSON string body (without the quotes) of `in`, cut to fit. */
static void json_escape(char *out, size_t size, const char *in)
{
    size_t o = 0;
    if (!size) return;
    for (; in && *in && o + 7 < size; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, size - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}

static int path_join(char *out, size_t size, const char *a, const char *b)
{
    int n = snprintf(out, size, "%s" SEP "%s", a, b);
    return n > 0 && (size_t)n < size;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static double percentile(double *v, size_t n, double p)
{
    size_t i;
    if (!n) return 0.0;
    qsort(v, n, sizeof *v, cmp_double);
    i = (size_t)(p * (double)(n - 1) + 0.5);
    return v[i < n ? i : n - 1];
}

/* ---- events ------------------------------------------------------------------------------------- */

static void store_event(double t, const char *line, int locked)
{
    size_t slot = (size_t)(g_ev_n % EVENT_RING);
    size_t len = strlen(line);
    char *copy = (char *)malloc(len + 1);
    if (copy) memcpy(copy, line, len + 1);
    if (g_events_file) fputs(line, g_events_file), fputc('\n', g_events_file);
    free(g_ev[slot]);
    g_ev[slot] = copy;
    g_ev_t[slot] = t;
    g_ev_n++;
    (void)locked;
}

static void note_event(const char *kind, const char *fields)
{
    if (!strcmp(kind, "fe") && fields) {
        /* "text":"..." -- the screen the front end last asked about. */
        const char *p = strstr(fields, "\"text\":\"");
        if (p) {
            char text[256];
            size_t i = 0;
            p += 8;
            while (*p && *p != '"' && i + 1 < sizeof text) {
                if (*p == '\\' && p[1]) p++;
                text[i++] = *p++;
            }
            text[i] = 0;
            {
                char lower[256];
                size_t k;
                for (k = 0; k <= i; k++) lower[k] = (char)tolower((unsigned char)text[k]);
                if (strstr(lower, "getscreeninfo(")) copy_text(g_last_screen, sizeof g_last_screen, text);
            }
        }
    } else if (!strcmp(kind, "game") && fields && strstr(fields, "match_start")) {
        copy_text(g_last_game, sizeof g_last_game, fields);
    }
}

void recorder_event(const char *kind, const char *fields)
{
    char line[4096];
    double t;
    int locked;
    if (!g_on || !kind) return;
    t = now_ms();
    snprintf(line, sizeof line, "{\"t_ms\":%.1f,\"frame\":%llu,\"kind\":\"%s\"%s%s}",
             rel_ms(t), g_frame, kind, fields && *fields ? "," : "", fields && *fields ? fields : "");
    if (g_crashed) {
        locked = mtx_trylock(&g_lock);
    } else {
        mtx_lock(&g_lock);
        locked = 1;
    }
    note_event(kind, fields);
    store_event(t, line, locked);
    if (locked) mtx_unlock(&g_lock);
}

void recorder_event_text(const char *kind, const char *key, const char *text)
{
    char esc[2048], fields[2200];
    if (!g_on) return;
    json_escape(esc, sizeof esc, text);
    snprintf(fields, sizeof fields, "\"%s\":\"%s\"", key ? key : "text", esc);
    recorder_event(kind, fields);
}

/* ---- the picture ring --------------------------------------------------------------------------- */

int recorder_picture_wanted(void)
{
    int mask = 0;
    if (!g_on || g_frozen) return 0;
    if (g_cfg.video_fps > 0 && now_ms() >= g_next_pic) mask |= 1;
    if (g_still_wanted) mask |= 2;
    g_pic_mask = mask;
    return mask;
}

/* Box-filter `rgba` (sw x sh) to `out` (w x h, R G B), applying `gamma`. `bgra`: the
 * source's bytes run B, G, R, A. */
static void downsample(const uint8_t *rgba, int pitch, int sw, int sh, uint8_t *out, int w, int h,
                       int bgra, const uint8_t gamma[3][256])
{
    int x, y;
    for (y = 0; y < h; y++) {
        int y0 = (int)((long long)y * sh / h), y1 = (int)((long long)(y + 1) * sh / h);
        if (y1 <= y0) y1 = y0 + 1;
        for (x = 0; x < w; x++) {
            int x0 = (int)((long long)x * sw / w), x1 = (int)((long long)(x + 1) * sw / w);
            unsigned sum[3] = { 0, 0, 0 }, n;
            int i, j, c;
            if (x1 <= x0) x1 = x0 + 1;
            /* At most a 4x4 sample of a large block: plenty for a thumbnail. */
            {
                int sy = (y1 - y0 + 3) / 4, sx = (x1 - x0 + 3) / 4;
                n = 0;
                for (j = y0; j < y1; j += sy) {
                    const uint8_t *row = rgba + (size_t)j * (size_t)pitch;
                    for (i = x0; i < x1; i += sx) {
                        for (c = 0; c < 3; c++) sum[c] += row[i * 4 + (bgra ? 2 - c : c)];
                        n++;
                    }
                }
            }
            for (c = 0; c < 3; c++) {
                unsigned v = sum[c] / (n ? n : 1);
                out[((size_t)y * (size_t)w + (size_t)x) * 3 + (size_t)c] = gamma ? gamma[c][v & 0xFF] : (uint8_t)v;
            }
        }
    }
}

/* The picture's period has been served: the next one is due a period later. */
static void picture_advance(double t)
{
    double period = 1000.0 / (double)(g_cfg.video_fps > 0 ? g_cfg.video_fps : 1);
    g_next_pic += period;
    if (g_next_pic < t) g_next_pic = t + period;
}

/* A picture for the ring (mask 1) and/or the still (mask 2), taken at `frame` and `t`.
 * `advance`: this call also serves the period (a synchronous picture; an asynchronous one
 * served it when it was requested). */
static void picture_store(int mask, unsigned long long frame, double t, const uint8_t *rgba, int pitch,
                          int width, int height, int bgra, const uint8_t gamma[3][256], int advance)
{
    if (!g_on || !rgba || width <= 0 || height <= 0 || !mask) return;
    /* A bundle being written holds the ring for a moment: skip this picture rather than
     * make the frame wait (the recorder must not cause the hitches it records). */
    if (!mtx_trylock(&g_pic_lock)) {
        if (mask & 2) g_still_wanted = 1;
        return;
    }
    if ((mask & 1) && !g_frozen) {
        if (!g_ring && g_cfg.video_fps > 0) {
            int th = g_cfg.thumb_height > 0 ? g_cfg.thumb_height : 240;
            int tw = (int)((double)th * (double)width / (double)height + 0.5);
            tw = (tw + 3) & ~3;
            g_ring_slots = g_cfg.video_fps * (g_cfg.ring_seconds + g_cfg.after_seconds);
            g_slot_bytes = (size_t)tw * (size_t)th * 3u;
            g_ring = (uint8_t *)malloc(g_slot_bytes * (size_t)g_ring_slots);
            g_ring_meta = (PicMeta *)calloc((size_t)g_ring_slots, sizeof(PicMeta));
            if (!g_ring || !g_ring_meta) {
                free(g_ring); free(g_ring_meta);
                g_ring = NULL; g_ring_meta = NULL; g_ring_slots = 0;
                g_cfg.video_fps = 0;
                fprintf(stderr, "[RECORD] no memory for the picture ring; recording without pictures\n");
            } else {
                g_tw = tw;
                g_th = th;
            }
        }
        if (g_ring) {
            size_t slot = (size_t)(g_ring_n % (unsigned long long)g_ring_slots);
            downsample(rgba, pitch, width, height, g_ring + slot * g_slot_bytes, g_tw, g_th, bgra, gamma);
            g_ring_meta[slot].frame = frame;
            g_ring_meta[slot].t = t;
            g_ring_n++;
        }
        if (advance)
            picture_advance(t);
    }
    if (mask & 2) {
        int h = 480, w = (int)(480.0 * (double)width / (double)height + 0.5);
        w = (w + 3) & ~3;
        if (!g_still || g_still_w != w) {
            free(g_still);
            g_still = (uint8_t *)malloc((size_t)w * (size_t)h * 3u);
            g_still_w = g_still ? w : 0;
            g_still_h = g_still ? h : 0;
        }
        if (g_still) {
            downsample(rgba, pitch, width, height, g_still, g_still_w, g_still_h, bgra, gamma);
            g_still_t = t;
            g_still_frame = frame;
            g_still_ready = 1;
        }
        g_still_wanted = 0;
    }
    mtx_unlock(&g_pic_lock);
}

void recorder_picture(const uint8_t *rgba, int pitch, int width, int height, const uint8_t gamma[3][256])
{
    int mask = g_pic_mask;
    g_pic_mask = 0;
    picture_store(mask, g_frame, now_ms(), rgba, pitch, width, height, 0, gamma, 1);
}

/* Asynchronous pictures: the request's time, by tag, until the picture arrives. */
#define PIC_PENDING 8
static struct { uint64_t tag; double t; } g_pic_pending[PIC_PENDING];
static unsigned g_pic_pending_i;

uint64_t recorder_picture_request(void)
{
    int mask = g_pic_mask;
    uint64_t tag;
    double t;
    g_pic_mask = 0;
    if (!g_on || !mask) return 0;
    t = now_ms();
    /* Served now, so the frames before the picture arrives do not ask again. A still
     * that then finds the ring busy asks again (picture_store puts the flag back). */
    if (mask & 1) picture_advance(t);
    if (mask & 2) g_still_wanted = 0;
    tag = ((uint64_t)g_frame << 2) | (uint64_t)mask;
    g_pic_pending[g_pic_pending_i % PIC_PENDING].tag = tag;
    g_pic_pending[g_pic_pending_i % PIC_PENDING].t = t;
    g_pic_pending_i++;
    return tag;
}

void recorder_picture_tagged(uint64_t tag, const uint8_t *pixels, int pitch, int width, int height, int bgra,
                             const uint8_t gamma[3][256])
{
    double t = -1.0;
    int i;
    for (i = 0; i < PIC_PENDING; i++)
        if (g_pic_pending[i].tag == tag) {
            t = g_pic_pending[i].t;
            g_pic_pending[i].tag = 0;
            break;
        }
    if (t < 0.0) t = now_ms();      /* too late to find: when it arrived */
    picture_store((int)(tag & 3u), (unsigned long long)(tag >> 2), t, pixels, pitch, width, height, bgra,
                  gamma, 0);
}

/* ---- frames --------------------------------------------------------------------------------------- */

void recorder_frame_begin(void)
{
    if (g_on) g_begin = now_ms();
}

static int counter_delta(int now, int *last)
{
    int d;
    if (*last < 0) { *last = now; return 0; }
    /* The runtime resets these each second while RECOMP_APU_LEVEL is on. */
    d = now >= *last ? now - *last : now;
    *last = now;
    return d;
}

static void recent_add(double interval)
{
    g_recent[g_recent_i] = interval;
    g_recent_i = (g_recent_i + 1) % RECENT;
    if (g_recent_n < RECENT) g_recent_n++;
    if (g_frame % 30 == 0 && g_recent_n >= 10) {
        double tmp[RECENT];
        memcpy(tmp, g_recent, sizeof(double) * (size_t)g_recent_n);
        g_median = percentile(tmp, (size_t)g_recent_n, 0.5);
    }
}

/* A hitch event, with what the probe saw while the frame was late (the guest CPU's
 * holder at the first probe and at the last). */
static void hitch_event(const char *fields, unsigned long long frame)
{
    static char big[2 * PROBE_HOLDER + 512];
    int n = 0;

    if (g_cfg.stall_probe) {
        mtx_lock(&g_lock);
        if (g_probe_frame == frame && g_probe_n > 0) {
            n = snprintf(big, sizeof big, "%s,\"probes\":%d,\"probe_since_ms\":%.1f,\"holder\":%s",
                         fields, g_probe_n, g_probe_since, g_probe_first[0] ? g_probe_first : "null");
            if (g_probe_n > 1 && n > 0 && (size_t)n < sizeof big)
                n += snprintf(big + n, sizeof big - (size_t)n, ",\"holder_last\":%s",
                              g_probe_last[0] ? g_probe_last : "null");
        }
        mtx_unlock(&g_lock);
    }
    recorder_event("hitch", n > 0 && (size_t)n < sizeof big ? big : fields);
}

void recorder_frame_end(const RecorderFrameIn *in)
{
    static uint32_t last_draws, last_vertices, last_clears, last_tex_uploads, last_tex_evictions;
    static unsigned long long last_tex_bytes;
    FrameRec r;
    double t;
    char fields[256];
    int hitch = 0;

    if (!g_on || !in) return;
    t = now_ms();
    memset(&r, 0, sizeof r);
    r.frame = g_frame;
    r.t = rel_ms(t);
    r.interval = g_last_end > 0.0 ? t - g_last_end : 0.0;
    r.present = in->present_ms;
    r.capture = in->capture_ms;
    r.work = g_last_end > 0.0 && g_begin > g_last_end ? g_begin - g_last_end : 0.0;
    r.draws = in->draws - last_draws;
    r.vertices = in->vertices - last_vertices;
    r.clears = in->clears - last_clears;
    last_draws = in->draws;
    last_vertices = in->vertices;
    last_clears = in->clears;
    r.tex_uploads = in->tex_uploads - last_tex_uploads;
    r.tex_evictions = in->tex_evictions - last_tex_evictions;
    r.tex_upload_kb = in->tex_upload_bytes >= last_tex_bytes ? (double)(in->tex_upload_bytes - last_tex_bytes) / 1024.0 : 0.0;
    last_tex_uploads = in->tex_uploads;
    last_tex_evictions = in->tex_evictions;
    last_tex_bytes = in->tex_upload_bytes;
    r.tex_cached = in->tex_cached;
    r.tex_cache_mb = (double)in->tex_cache_bytes / 1048576.0;
    r.hd_cache_mb = (double)in->hd_cache_bytes / 1048576.0;
    r.dropped = counter_delta(in->audio_dropped, &g_last_dropped);
    r.dry = counter_delta(in->audio_dry, &g_last_dry);
    r.step = in->step;
    r.log_bytes = in->log_bytes;
    r.pcm_bytes = g_audio_path[0] ? file_size(g_audio_path) : -1;
    g_total_dropped += r.dropped;
    g_total_dry += r.dry;

    mtx_lock(&g_lock);
    if (g_hist) {
        g_hist[g_hist_n % g_hist_cap] = r;
        g_hist_n++;
    }
    if (g_frames_file)
        fprintf(g_frames_file, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%d,%d,%lld,%lld,%lld,%u,%.1f,%u,%u,%.2f,%.2f\n",
                r.frame, r.t, r.interval, r.present, r.work, r.capture, r.draws, r.vertices, r.clears,
                r.dropped, r.dry, r.step, r.log_bytes, r.pcm_bytes,
                r.tex_uploads, r.tex_upload_kb, r.tex_evictions, r.tex_cached, r.tex_cache_mb, r.hd_cache_mb);
    mtx_unlock(&g_lock);

    if (r.interval > 0.0) {
        /* A frame is a hitch when it is long in itself and long against the recent
         * median: loading screens that run slowly for seconds are not one hitch
         * per frame. The recorder's own read-back is taken off first. */
        double own = r.interval - r.capture;
        if (g_frame > 60 && own >= (double)g_cfg.hitch_ms && own >= g_median * 2.5) {
            hitch = 1;
            g_hitches++;
            snprintf(fields, sizeof fields,
                     "\"interval_ms\":%.2f,\"median_ms\":%.2f,\"present_ms\":%.2f,\"work_ms\":%.2f,\"capture_ms\":%.2f",
                     r.interval, g_median, r.present, r.work, r.capture);
            hitch_event(fields, r.frame);
            if (g_cfg.auto_bundle_hitch_ms > 0 && own >= (double)g_cfg.auto_bundle_hitch_ms &&
                g_auto_bundles < 5 && (g_last_auto == 0.0 || t - g_last_auto > 60000.0)) {
                g_last_auto = t;
                g_auto_bundles++;
                recorder_mark("hitch");
            }
        }
        if (!hitch) recent_add(r.interval);
    }
    if (r.dropped > 0) {
        snprintf(fields, sizeof fields, "\"count\":%d,\"total\":%lld", r.dropped, g_total_dropped);
        recorder_event("audio_dropped", fields);
    }
    if (r.dry > 0) {
        snprintf(fields, sizeof fields, "\"count\":%d,\"total\":%lld", r.dry, g_total_dry);
        recorder_event("audio_dry", fields);
    }
    g_last_end = t;
    g_frame++;
}

/* ---- bundles -------------------------------------------------------------------------------------- */

static void write_bmp(const char *path, const uint8_t *rgb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    uint8_t hdr[54];
    int row = (w * 3 + 3) & ~3, y, x;
    uint32_t size = 54u + (uint32_t)row * (uint32_t)h;
    if (!f) return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    hdr[2] = (uint8_t)size; hdr[3] = (uint8_t)(size >> 8); hdr[4] = (uint8_t)(size >> 16); hdr[5] = (uint8_t)(size >> 24);
    hdr[10] = 54; hdr[14] = 40;
    hdr[18] = (uint8_t)w; hdr[19] = (uint8_t)(w >> 8);
    hdr[22] = (uint8_t)h; hdr[23] = (uint8_t)(h >> 8);
    hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = h - 1; y >= 0; y--) {
        const uint8_t *p = rgb + (size_t)y * (size_t)w * 3u;
        int written = 0;
        for (x = 0; x < w; x++, written += 3) {
            fputc(p[x * 3 + 2], f);
            fputc(p[x * 3 + 1], f);
            fputc(p[x * 3 + 0], f);
        }
        while (written++ < row) fputc(0, f);
    }
    fclose(f);
}

/* The last `bytes` of `src` into `dst`; returns the offset in `src` they start at, or -1. */
static long long copy_tail(const char *src, const char *dst, long long bytes, int align)
{
    FILE *in, *out;
    long long size = file_size(src), start;
    char buf[65536];
    size_t n;
    if (size < 0) return -1;
    start = size > bytes ? size - bytes : 0;
    if (align > 1) start -= start % align;
    in = fopen(src, "rb");
    if (!in) return -1;
    out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
#if defined(_WIN32)
    _fseeki64(in, start, SEEK_SET);
#else
    fseeko(in, (off_t)start, SEEK_SET);
#endif
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
    return start;
}

#if defined(_WIN32)
typedef BOOL (WINAPI *MiniDumpFn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);
typedef struct { DWORD ThreadId; PEXCEPTION_POINTERS ExceptionPointers; BOOL ClientPointers; } RecMinidumpException;
static int write_minidump(const char *path, void *exception_pointers)
{
    HMODULE dbg = LoadLibraryA("dbghelp.dll");
    MiniDumpFn fn = dbg ? (MiniDumpFn)(void (*)(void))GetProcAddress(dbg, "MiniDumpWriteDump") : NULL;
    HANDLE f;
    RecMinidumpException ex;
    BOOL ok;
    if (!fn) return 0;
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    ex.ThreadId = GetCurrentThreadId();
    ex.ExceptionPointers = (PEXCEPTION_POINTERS)exception_pointers;
    ex.ClientPointers = FALSE;
    /* MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules: every thread's stack and
     * registers, a few megabytes; guest memory goes separately (guest-ram.bin). */
    ok = fn(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1000 | 0x20,
            exception_pointers ? &ex : NULL, NULL, NULL);
    CloseHandle(f);
    return ok ? 1 : 0;
}
#endif

/* A window of `rows` from the frame history, oldest first, for t >= from. */
static size_t history_window(double from, FrameRec *out, size_t max)
{
    unsigned long long first = g_hist_n > g_hist_cap ? g_hist_n - g_hist_cap : 0, i;
    size_t n = 0;
    for (i = first; i < g_hist_n && n < max; i++) {
        const FrameRec *r = &g_hist[i % g_hist_cap];
        if (r->t >= from) out[n++] = *r;
    }
    return n;
}

static void write_bundle(const char *reason, double t_mark, unsigned long long frame_at_mark, int crash,
                         const char *what, void *os_exception)
{
    char dir[PATH_LEN], path[PATH_LEN + 64], name[128], stamp[32];
    double from = rel_ms(t_mark) - (double)g_cfg.ring_seconds * 1000.0;
    double until = rel_ms(now_ms());
    FrameRec *rows = NULL;
    size_t nrows = 0, i;
    unsigned long long first_pic = 0, npics = 0;
    long long log_offset = -1, audio_offset = -1;
    int have_still = 0, locked;
    FILE *f;
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    char esc[1200];

    strftime(stamp, sizeof stamp, "%H%M%S", tmv);
    snprintf(name, sizeof name, "%03d-%s-%s", g_bundles + 1, reason, stamp);
    {
        char parent[PATH_LEN];
        if (!path_join(parent, sizeof parent, g_session, "bundles")) return;
        make_dir(parent);
        if (!path_join(dir, sizeof dir, parent, name) || !make_dir(dir)) return;
    }
    g_bundles++;

    /* Pictures: freeze the ring, write it oldest first, let it run again. */
    g_frozen = 1;
    locked = crash ? mtx_trylock(&g_pic_lock) : (mtx_lock(&g_pic_lock), 1);
    if (g_ring && g_ring_n) {
        unsigned long long k;
        first_pic = g_ring_n > (unsigned long long)g_ring_slots ? g_ring_n - (unsigned long long)g_ring_slots : 0;
        npics = g_ring_n - first_pic;
        if (path_join(path, sizeof path, dir, "clip.rgb") && (f = fopen(path, "wb")) != NULL) {
            for (k = first_pic; k < g_ring_n; k++)
                fwrite(g_ring + (size_t)(k % (unsigned long long)g_ring_slots) * g_slot_bytes, 1, g_slot_bytes, f);
            fclose(f);
        }
    }
    if (g_still_ready && g_still) {
        if (path_join(path, sizeof path, dir, "still.bmp")) write_bmp(path, g_still, g_still_w, g_still_h);
        have_still = 1;
    }
    /* The ring's index is written below, from a copy taken while it is still frozen. */
    {
        PicMeta *meta = NULL;
        if (npics) {
            meta = (PicMeta *)malloc(sizeof(PicMeta) * (size_t)npics);
            if (meta) {
                unsigned long long k;
                for (k = 0; k < npics; k++)
                    meta[k] = g_ring_meta[(first_pic + k) % (unsigned long long)g_ring_slots];
            }
        }
        g_still_ready = 0;
        if (locked) mtx_unlock(&g_pic_lock);
        g_frozen = 0;

        /* Rows and events of the same seconds: copied under the lock (the presenting thread
         * takes it each frame), written after it. */
        rows = (FrameRec *)malloc(sizeof(FrameRec) * (g_hist_cap ? g_hist_cap : 1));
        {
            char *events = NULL;
            size_t events_len = 0, events_cap = 0;
            locked = crash ? mtx_trylock(&g_lock) : (mtx_lock(&g_lock), 1);
            if (rows && g_hist) nrows = history_window(from, rows, g_hist_cap);
            {
                unsigned long long k = g_ev_n > EVENT_RING ? g_ev_n - EVENT_RING : 0;
                for (; k < g_ev_n; k++) {
                    size_t slot = (size_t)(k % EVENT_RING), n;
                    if (!g_ev[slot] || rel_ms(g_ev_t[slot]) < from) continue;
                    n = strlen(g_ev[slot]);
                    if (events_len + n + 2 > events_cap) {
                        size_t cap = (events_cap ? events_cap * 2 : 65536) + n + 2;
                        char *grown = (char *)realloc(events, cap);
                        if (!grown) break;
                        events = grown;
                        events_cap = cap;
                    }
                    memcpy(events + events_len, g_ev[slot], n);
                    events_len += n;
                    events[events_len++] = '\n';
                }
            }
            if (locked) mtx_unlock(&g_lock);
            if (g_frames_file) fflush(g_frames_file);
            if (g_events_file) fflush(g_events_file);
            if (path_join(path, sizeof path, dir, "frames.csv") && (f = fopen(path, "w")) != NULL) {
                fputs("frame,t_ms,interval_ms,present_ms,work_ms,capture_ms,draws,vertices,clears,"
                      "audio_dropped,audio_dry,step,log_bytes,pcm_bytes,"
                      "tex_uploads,tex_upload_kb,tex_evictions,tex_cached,tex_cache_mb,hd_cache_mb\n", f);
                for (i = 0; i < nrows; i++) {
                    const FrameRec *r = &rows[i];
                    fprintf(f, "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%d,%d,%lld,%lld,%lld,%u,%.1f,%u,%u,%.2f,%.2f\n",
                            r->frame, r->t, r->interval, r->present, r->work, r->capture, r->draws, r->vertices,
                            r->clears, r->dropped, r->dry, r->step, r->log_bytes, r->pcm_bytes,
                            r->tex_uploads, r->tex_upload_kb, r->tex_evictions, r->tex_cached, r->tex_cache_mb,
                            r->hd_cache_mb);
                }
                fclose(f);
            }
            if (path_join(path, sizeof path, dir, "events.jsonl") && (f = fopen(path, "wb")) != NULL) {
                if (events_len) fwrite(events, 1, events_len, f);
                fclose(f);
            }
            free(events);
        }

        /* The end of the log and of the audio. */
        fflush(stderr);
        if (g_log_path[0] && path_join(path, sizeof path, dir, "log-tail.txt"))
            log_offset = copy_tail(g_log_path, path, 1 << 20, 1);
        if (g_audio_path[0] && path_join(path, sizeof path, dir, "audio.pcm"))
            audio_offset = copy_tail(g_audio_path, path,
                                     (long long)(g_cfg.ring_seconds + g_cfg.after_seconds + 1) * 192000LL, 4);

        if (crash) {
#if defined(_WIN32)
            if (path_join(path, sizeof path, dir, "process.dmp")) write_minidump(path, os_exception);
#else
            (void)os_exception;
#endif
        }
        if ((crash || !strcmp(reason, "hang")) && g_cfg.guest_ram && g_cfg.guest_base && g_cfg.guest_size) {
            const void *base = g_cfg.guest_base();
            if (base && path_join(path, sizeof path, dir, "guest-ram.bin") && (f = fopen(path, "wb")) != NULL) {
                fwrite(base, 1, g_cfg.guest_size, f);
                fclose(f);
            }
        }

        /* bundle.json: what this is, where it came from, and the index of the pictures. */
        if (path_join(path, sizeof path, dir, "bundle.json") && (f = fopen(path, "w")) != NULL) {
            double *iv = rows && nrows ? (double *)malloc(sizeof(double) * nrows) : NULL;
            double median = 0, p99 = 0, worst = 0;
            int dropped = 0, dry = 0;
            size_t n = 0;
            for (i = 0; i < nrows; i++) {
                dropped += rows[i].dropped;
                dry += rows[i].dry;
                if (rows[i].interval > 0 && iv) iv[n++] = rows[i].interval;
                if (rows[i].interval > worst) worst = rows[i].interval;
            }
            if (iv && n) {
                median = percentile(iv, n, 0.5);
                p99 = percentile(iv, n, 0.99);
            }
            free(iv);
            fprintf(f, "{\n  \"format\": \"defjam-bug-bundle/1\",\n");
            json_escape(esc, sizeof esc, reason);
            fprintf(f, "  \"reason\": \"%s\",\n", esc);
            json_escape(esc, sizeof esc, what ? what : "");
            fprintf(f, "  \"detail\": \"%s\",\n", esc);
            fprintf(f, "  \"mark_t_ms\": %.1f,\n  \"written_t_ms\": %.1f,\n  \"window_from_t_ms\": %.1f,\n",
                    rel_ms(t_mark), until, from);
            fprintf(f, "  \"frame_at_mark\": %llu,\n", frame_at_mark);
            json_escape(esc, sizeof esc, g_build);
            fprintf(f, "  \"build\": \"%s\",\n", esc);
#if defined(_WIN32)
            fprintf(f, "  \"platform\": \"windows\",\n");
#elif defined(__APPLE__)
            fprintf(f, "  \"platform\": \"macos\",\n");
#else
            fprintf(f, "  \"platform\": \"linux\",\n");
#endif
            fprintf(f, "  \"pid\": %d,\n", process_id());
            json_escape(esc, sizeof esc, g_session);
            fprintf(f, "  \"session\": \"%s\",\n", esc);
            json_escape(esc, sizeof esc, g_last_screen);
            fprintf(f, "  \"last_screen\": \"%s\",\n", esc);
            fprintf(f, "  \"view_mode\": %d,\n", g_view_mode);
            fprintf(f, "  \"last_match_start\": %s%s%s,\n", g_last_game[0] ? "{" : "null",
                    g_last_game[0] ? g_last_game : "", g_last_game[0] ? "}" : "");
            fprintf(f, "  \"settings\": %s,\n", g_settings_json ? g_settings_json : "{}");
            fprintf(f, "  \"summary\": {\"frames\": %u, \"median_ms\": %.3f, \"p99_ms\": %.3f, \"worst_ms\": %.3f, "
                       "\"audio_dropped\": %d, \"audio_dry\": %d, \"hitches_in_session\": %lld},\n",
                    (unsigned)nrows, median, p99, worst, dropped, dry, g_hitches);
            fprintf(f, "  \"log_tail\": %s, \"log_tail_offset\": %lld,\n", log_offset >= 0 ? "\"log-tail.txt\"" : "null",
                    log_offset);
            fprintf(f, "  \"audio\": %s, \"audio_offset\": %lld, \"audio_format\": \"s16le 48000 Hz stereo\",\n",
                    audio_offset >= 0 ? "\"audio.pcm\"" : "null", audio_offset);
            fprintf(f, "  \"still\": %s,\n", have_still ? "\"still.bmp\"" : "null");
            fprintf(f, "  \"clip\": {\"file\": %s, \"format\": \"rgb24\", \"width\": %d, \"height\": %d, \"frames\": [",
                    npics ? "\"clip.rgb\"" : "null", g_tw, g_th);
            if (meta) {
                unsigned long long k;
                for (k = 0; k < npics; k++)
                    fprintf(f, "%s[%llu, %.1f]", k ? ", " : "", meta[k].frame, rel_ms(meta[k].t));
            }
            fprintf(f, "]}\n}\n");
            fclose(f);
        }
        free(meta);
    }
    free(rows);

    if (path_join(path, sizeof path, dir, "README.txt") && (f = fopen(path, "w")) != NULL) {
        fprintf(f,
                "A play-test bug bundle (%s). It holds the game's own pictures and sound: keep it local,\n"
                "never commit or upload it.\n\n"
                "Read it with:   python scripts/bug-bundle.py \"%s\"\n\n"
                "That writes report.md (start there), a contact sheet, a frame-time chart, an audio\n"
                "picture and the log lines of these seconds with their times, beside these files.\n", reason, dir);
        if (!strcmp(reason, "hang"))
            fprintf(f, "\nThe game was still running when this was written (pid %d). While it is, run\n"
                       "  python scripts/native-stacks.py --pid %d\n"
#if defined(_WIN32)
                       "  python scripts/sample-threads.py --pid %d\n"
#endif
                       "to see where each thread is.\n", process_id(), process_id()
#if defined(_WIN32)
                       , process_id()
#endif
                       );
        fclose(f);
    }
    copy_text(g_last_bundle, sizeof g_last_bundle, dir);
    fprintf(stderr, "[RECORD] bundle (%s) written to %s\n", reason, dir);
    fflush(stderr);
    if (!crash) {
        char fields[sizeof esc + 64];
        json_escape(esc, sizeof esc, dir);
        snprintf(fields, sizeof fields, "\"reason\":\"%s\",\"dir\":\"%s\"", reason, esc);
        recorder_event("bundle", fields);
    }
}

void recorder_mark(const char *reason)
{
    char fields[96];
    if (!g_on || g_mark_pending) return;
    copy_text(g_mark_reason, sizeof g_mark_reason, reason && *reason ? reason : "mark");
    g_mark_t = now_ms();
    g_mark_frame = g_frame;
    g_still_wanted = 1;
    g_mark_pending = 1;
    snprintf(fields, sizeof fields, "\"reason\":\"%s\"", g_mark_reason);
    recorder_event("mark", fields);
}

void recorder_on_crash(const char *what)
{
    if (!g_on || g_crashed) return;
    g_crashed = 1;
    recorder_event_text("crash", "what", what);
    write_bundle("crash", now_ms(), g_frame, 1, what, NULL);
}

/* The Windows exception handler's entry: as recorder_on_crash, with the
 * exception for the minidump. */
void recorder_on_crash_os(const char *what, void *os_exception)
{
    if (!g_on || g_crashed) return;
    g_crashed = 1;
    recorder_event_text("crash", "what", what);
    write_bundle("crash", now_ms(), g_frame, 1, what, os_exception);
}

/* ---- the worker ------------------------------------------------------------------------------------- */

static void worker_tick(void)
{
    static double last_flush, last_memory;
    static int hang_reported;
    double t = now_ms();

    if (g_mark_pending) {
        int is_auto = !strcmp(g_mark_reason, "hitch");
        double wait = is_auto ? (double)g_cfg.after_seconds * 500.0 : (double)g_cfg.after_seconds * 1000.0;
        /* Wait for the seconds after the mark, and for the still (or give up on it). */
        if (t - g_mark_t >= wait && (g_still_ready || t - g_mark_t >= wait + 2000.0)) {
            write_bundle(g_mark_reason, g_mark_t, g_mark_frame, 0, NULL, NULL);
            g_mark_pending = 0;
        }
    }
    if (g_cfg.hang_seconds > 0 && g_last_end > 0.0) {
        if (t - g_last_end >= (double)g_cfg.hang_seconds * 1000.0) {
            if (!hang_reported) {
                char fields[64];
                hang_reported = 1;
                snprintf(fields, sizeof fields, "\"since_ms\":%.0f", t - g_last_end);
                recorder_event("hang", fields);
                write_bundle("hang", t, g_frame, 0, "no frame presented", NULL);
            }
        } else if (hang_reported) {
            hang_reported = 0;
            recorder_event("hang_over", NULL);
        }
    }
    if (t - last_memory >= 5000.0) {
        long long bytes = private_bytes();
        char fields[64];
        last_memory = t;
        if (bytes >= 0) {
            snprintf(fields, sizeof fields, "\"private_mb\":%.1f", (double)bytes / 1048576.0);
            recorder_event("memory", fields);
        }
    }
    if (t - last_flush >= 1000.0) {
        last_flush = t;
        /* Not under g_lock: each FILE has its own lock, and a slow disk must not hold up a frame. */
        if (g_frames_file) fflush(g_frames_file);
        if (g_events_file) fflush(g_events_file);
    }
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID arg)
#else
static void *worker_entry(void *arg)
#endif
{
    (void)arg;
    while (!g_stop) {
        worker_tick();
        sleep_ms(50);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* ---- the hitch probe -------------------------------------------------------------------------------- */

/* Looks every few milliseconds for a frame that is late, and asks the runtime what
 * its threads are doing while it still is: a stall that has ended leaves nothing to see. */
#if defined(_WIN32)
static DWORD WINAPI prober_entry(LPVOID arg)
#else
static void *prober_entry(void *arg)
#endif
{
    static char holder[PROBE_HOLDER], all[256 * 1024];
    unsigned long long seen = ~0ull;
    int n = 0;
    double last = 0.0, woke = now_ms(), gap_max = 0.0;

    (void)arg;
    while (!g_stop) {
        double t, end, since, gap;
        unsigned long long frame;
        int n_ok;

        sleep_ms(4);
        end = g_last_end;
        frame = g_frame;
        t = now_ms();
        /* How long this thread was away for a 4 ms sleep: a stall that stops every
         * thread of the process (or the host's scheduling of it) shows here too. */
        gap = t - woke;
        woke = t;
        if (frame != seen) { seen = frame; n = 0; gap_max = 0.0; }
        if (gap > gap_max) gap_max = gap;
        if (end <= 0.0 || frame < 60) continue;
        since = t - end;
        if (since < (double)g_cfg.stall_probe_ms || n >= 6 || (n > 0 && t - last < 50.0)) continue;
        n_ok = g_cfg.stall_probe(holder, sizeof holder, all, sizeof all);
        woke = now_ms();                        /* the probe's own time is not a late wake-up */
        if (n_ok <= 0) continue;
        if (g_frame != frame) continue;         /* the frame came while we looked */
        n++;
        last = t;
        mtx_lock(&g_lock);
        if (g_probe_frame != frame) {
            g_probe_frame = frame;
            g_probe_since = since;
            copy_text(g_probe_first, sizeof g_probe_first, holder);
            g_probe_last[0] = 0;
        } else {
            copy_text(g_probe_last, sizeof g_probe_last, holder);
        }
        g_probe_n = n;
        mtx_unlock(&g_lock);
        if (g_stalls_file) {
            fprintf(g_stalls_file, "{\"t_ms\":%.1f,\"frame\":%llu,\"since_ms\":%.1f,\"probe\":%d,"
                                   "\"cost_ms\":%.2f,\"prober_gap_ms\":%.1f,\"holder\":%s,\"all\":%s}\n",
                    rel_ms(t), frame, since, n, now_ms() - t, gap_max, holder[0] ? holder : "null",
                    all[0] ? all : "null");
            fflush(g_stalls_file);
        }
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

/* ---- the session ---------------------------------------------------------------------------------- */

typedef struct Sessions { char names[512][64]; int n; } Sessions;

static void collect_session(const char *name, void *user)
{
    Sessions *s = (Sessions *)user;
    if (s->n < 512 && strlen(name) < 64 && isdigit((unsigned char)name[0])) {
        copy_text(s->names[s->n], 64, name);
        s->n++;
    }
}

static int cmp_name(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

/* Older sessions keep their bundles, events and session.json; their frame rows and
 * audio, which are most of their size, go. */
static void prune_sessions(const char *parent, int keep)
{
    Sessions *s = (Sessions *)calloc(1, sizeof(Sessions));
    int i;
    if (!s) return;
    list_dir(parent, collect_session, s);
    qsort(s->names, (size_t)s->n, sizeof s->names[0], cmp_name);
    for (i = 0; i < s->n - keep; i++) {
        char dir[PATH_LEN], path[PATH_LEN + 32];
        if (!path_join(dir, sizeof dir, parent, s->names[i])) continue;
        if (path_join(path, sizeof path, dir, "audio.pcm")) remove(path);
        if (path_join(path, sizeof path, dir, "frames.csv")) remove(path);
    }
    free(s);
}

int recorder_init(const RecorderConfig *cfg)
{
    char parent[PATH_LEN], path[PATH_LEN + 32], stamp[32], esc[PATH_LEN * 2];
    time_t now = time(NULL);
    struct tm *tmv = localtime(&now);
    FILE *f;

    if (g_on || !cfg || !cfg->enabled) return g_on;
    g_cfg = *cfg;
    if (g_cfg.ring_seconds <= 0) g_cfg.ring_seconds = 30;
    if (g_cfg.ring_seconds > 300) g_cfg.ring_seconds = 300;
    if (g_cfg.after_seconds < 0) g_cfg.after_seconds = 0;
    if (g_cfg.video_fps < 0) g_cfg.video_fps = 0;
    if (g_cfg.video_fps > 60) g_cfg.video_fps = 60;
    if (g_cfg.hitch_ms <= 0) g_cfg.hitch_ms = 50;
    if (g_cfg.keep_sessions <= 0) g_cfg.keep_sessions = 20;
    copy_text(g_root, sizeof g_root, cfg->root && *cfg->root ? cfg->root : "logs");
    copy_text(g_log_path, sizeof g_log_path, cfg->log_path);
    copy_text(g_build, sizeof g_build, cfg->build);
    if (cfg->settings_json) {
        size_t n = strlen(cfg->settings_json);
        g_settings_json = (char *)malloc(n + 1);
        if (g_settings_json) memcpy(g_settings_json, cfg->settings_json, n + 1);
    }
    g_cfg.root = g_root;
    g_cfg.log_path = g_log_path;
    g_cfg.build = g_build;
    g_cfg.settings_json = g_settings_json;

    make_dir(g_root);
    if (!path_join(parent, sizeof parent, g_root, "sessions")) return 0;
    make_dir(parent);
    prune_sessions(parent, g_cfg.keep_sessions - 1);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", tmv);
    if (!path_join(g_session, sizeof g_session, parent, stamp) || !make_dir(g_session)) {
        fprintf(stderr, "[RECORD] could not make %s; not recording\n", g_session);
        return 0;
    }
    if (cfg->audio_path && *cfg->audio_path)
        copy_text(g_audio_path, sizeof g_audio_path, cfg->audio_path);

    mtx_init(&g_lock);
    mtx_init(&g_pic_lock);
    g_hist_cap = (size_t)(g_cfg.ring_seconds + g_cfg.after_seconds + 2) * 250u;   /* up to 250 frames a second */
    g_hist = (FrameRec *)calloc(g_hist_cap, sizeof(FrameRec));
    if (path_join(path, sizeof path, g_session, "frames.csv") && (g_frames_file = fopen(path, "w")) != NULL)
        fputs("frame,t_ms,interval_ms,present_ms,work_ms,capture_ms,draws,vertices,clears,"
              "audio_dropped,audio_dry,step,log_bytes,pcm_bytes,"
                      "tex_uploads,tex_upload_kb,tex_evictions,tex_cached,tex_cache_mb,hd_cache_mb\n", g_frames_file);
    if (path_join(path, sizeof path, g_session, "events.jsonl"))
        g_events_file = fopen(path, "w");
    g_t0 = now_ms();
    g_next_pic = g_t0;
    g_on = 1;

    if (path_join(path, sizeof path, g_session, "session.json") && (f = fopen(path, "w")) != NULL) {
        char date[64];
        strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", tmv);
        fprintf(f, "{\n  \"format\": \"defjam-session/1\",\n  \"started\": \"%s\",\n", date);
        json_escape(esc, sizeof esc, g_build);
        fprintf(f, "  \"build\": \"%s\",\n", esc);
        json_escape(esc, sizeof esc, g_log_path);
        fprintf(f, "  \"log\": \"%s\",\n", esc);
        json_escape(esc, sizeof esc, g_audio_path);
        fprintf(f, "  \"audio\": \"%s\",\n", esc);
        fprintf(f, "  \"pid\": %d,\n", process_id());
#if defined(_WIN32)
        fprintf(f, "  \"platform\": \"windows\",\n");
#elif defined(__APPLE__)
        fprintf(f, "  \"platform\": \"macos\",\n");
#else
        fprintf(f, "  \"platform\": \"linux\",\n");
#endif
        fprintf(f, "  \"recorder\": {\"video_fps\": %d, \"ring_seconds\": %d, \"thumb_height\": %d, "
                   "\"after_seconds\": %d, \"hitch_ms\": %d, \"auto_bundle_hitch_ms\": %d, \"hang_seconds\": %d},\n",
                g_cfg.video_fps, g_cfg.ring_seconds, g_cfg.thumb_height, g_cfg.after_seconds, g_cfg.hitch_ms,
                g_cfg.auto_bundle_hitch_ms, g_cfg.hang_seconds);
        fprintf(f, "  \"settings\": %s\n}\n", g_settings_json ? g_settings_json : "{}");
        fclose(f);
    }
    if (path_join(path, sizeof path, parent, "LATEST") && (f = fopen(path, "w")) != NULL) {
        fprintf(f, "%s\n", stamp);
        fclose(f);
    }
    recorder_event("session_start", NULL);
    start_worker();
    if (g_cfg.stall_probe && g_cfg.stall_probe_ms > 0) {
        if (path_join(path, sizeof path, g_session, "stalls.jsonl"))
            g_stalls_file = fopen(path, "w");
        start_prober();
        fprintf(stderr, "[RECORD] hitch probe on: a frame %d ms late is sampled into stalls.jsonl\n",
                g_cfg.stall_probe_ms);
    }
    if (cfg->mark_key_name && *cfg->mark_key_name)
        fprintf(stderr, "[RECORD] recording this session to %s (pictures %d a second, %d s kept; mark with %s)\n",
                g_session, g_cfg.video_fps, g_cfg.ring_seconds, cfg->mark_key_name);
    else
        fprintf(stderr, "[RECORD] recording this session to %s (pictures %d a second, %d s kept; mark with the key %d)\n",
                g_session, g_cfg.video_fps, g_cfg.ring_seconds, g_cfg.mark_vk);
    fflush(stderr);
    return 1;
}

int recorder_on(void) { return g_on; }
double recorder_now_ms(void) { return now_ms(); }

void recorder_set_audio_path(const char *path)
{
    char fields[PATH_LEN * 2 + 16], esc[PATH_LEN * 2];
    if (!g_on || !path) return;
    copy_text(g_audio_path, sizeof g_audio_path, path);
    json_escape(esc, sizeof esc, path);
    snprintf(fields, sizeof fields, "\"path\":\"%s\"", esc);
    recorder_event("audio_file", fields);
}
const char *recorder_session_dir(void) { return g_on ? g_session : ""; }
int recorder_mark_vk(void) { return g_on ? g_cfg.mark_vk : 0; }

void recorder_set_view_mode(int mode)
{
    static int candidate = -1, seen;
    char fields[32];
    if (!g_on) return;
    /* Reported once it has held for 30 calls in a row: a frame can set up more than
     * one full-screen view, and a flip each call would be an event a call. */
    if (mode != candidate) {
        candidate = mode;
        seen = 0;
    }
    if (++seen != 30 || mode == g_view_mode) return;
    g_view_mode = mode;
    snprintf(fields, sizeof fields, "\"mode\":%d,\"cutscene\":%s", mode, mode == 1 ? "true" : "false");
    recorder_event("view", fields);
}

void recorder_status(char *out, size_t size)
{
    if (!size) return;
    if (!g_on) {
        snprintf(out, size, "Not recording.");
        return;
    }
    snprintf(out, size, "Recording: %llu frames, %lld hitches, %lld audio drop-outs, %d bundle(s)%s. Folder: %s",
             g_frame, g_hitches, g_total_dropped + g_total_dry, g_bundles,
             g_mark_pending ? " (one being written)" : "", g_session);
}

void recorder_shutdown(void)
{
    if (!g_on) return;
    recorder_event("session_end", NULL);
    g_stop = 1;
    mtx_lock(&g_lock);
    if (g_frames_file) fclose(g_frames_file);
    if (g_events_file) fclose(g_events_file);
    g_frames_file = g_events_file = NULL;
    mtx_unlock(&g_lock);
}
