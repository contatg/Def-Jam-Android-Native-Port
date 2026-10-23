/*
 * The [debug] settings and the play-test recorder's start-up (recorder.c).
 *
 *   [debug] record = true      or RECOMP_RECORD=1 for one run
 *
 * keeps a session folder under logs/sessions/ for every run: frame times,
 * events, the mixed audio and the last seconds of pictures, and F9 (mark_key)
 * writes a bug bundle. docs/playtest-recorder.md is the guide. Off by default,
 * and off in harness runs (RECOMP_SETTINGS=none) unless RECOMP_RECORD is set.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp_settings.h"
#include "input_map.h"
#include "pc_debug.h"
#include "recorder.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <limits.h>                     /* PATH_MAX: Darwin defines it only in <sys/syslimits.h> */
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef DEFJAM_VERSION
#define DEFJAM_VERSION "unknown"
#endif

extern ptrdiff_t g_xbox_mem_offset;
#define GUEST_RAM_BYTES 0x04000000u     /* the console's 64 MB, guest addresses 0 up */

static const void *guest_base(void)
{
    return g_xbox_mem_offset ? (const void *)g_xbox_mem_offset : NULL;
}

#if !defined(_WIN32)
/* The runtime's hitch probe (win32_compat.c; Darwin only, 0 elsewhere). */
int guest_turn_probe(char *holder, size_t holder_size, char *all, size_t all_size);
#endif

/* RECOMP_HITCH_PROBE=1 (or a number of milliseconds, 60 for 1): when a frame is that
 * late, sample which thread holds the guest CPU and every thread's stack, into the
 * hitch event and stalls.jsonl. Off by default. */
static int hitch_probe_ms(void)
{
    const char *e = getenv("RECOMP_HITCH_PROBE");
    int ms = e ? atoi(e) : 0;
    if (ms == 1) ms = 60;
    return ms > 0 ? ms : 0;
}

size_t pc_debug_settings(RecompSetting *out, size_t max)
{
    size_t n = 0;
    if (max < 11) return 0;
    memset(out, 0, sizeof(*out) * 11);
#define ADD(...) do { RecompSetting s_ = { __VA_ARGS__ }; out[n++] = s_; } while (0)
    ADD("debug", "record", RECOMP_SETTING_BOOL, 0, 0, 0, NULL, "RECOMP_RECORD", RECOMP_SETTING_RESTART,
        "Keep a play-test record of every run under logs/sessions: frame times, events, the sound and the last "
        "seconds of pictures. The mark key writes a bug bundle of those seconds. See docs/playtest-recorder.md.");
    ADD("debug", "mark_key", RECOMP_SETTING_STRING, 0, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
        "While recording, the key that marks a moment and writes a bug bundle of the seconds around it.", "F9");
    ADD("debug", "mark_pad", RECOMP_SETTING_STRING, 0, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
        "While recording, pad buttons that mark a moment, for a pad or a Steam Deck. Alternatives are separated "
        "by commas; a combination joined with + must be held for half a second. Button names as in [gamepad]. "
        "Empty for none.", "back+left_stick_click");
    ADD("debug", "record_fps", RECOMP_SETTING_INT, 10, 0, 30, NULL, "RECOMP_RECORD_FPS", RECOMP_SETTING_RESTART,
        "Pictures a second kept for bundles (0 for none). Each one is read back from the graphics card, which "
        "costs that frame a little; frames.csv records how much.");
    ADD("debug", "record_seconds", RECOMP_SETTING_INT, 30, 5, 120, NULL, "RECOMP_RECORD_SECONDS",
        RECOMP_SETTING_RESTART, "How many seconds before a mark a bundle holds.");
    ADD("debug", "record_audio", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
        "Also keep the mixed sound (about 11 MB a minute; older sessions lose it, see keep_sessions).");
    ADD("debug", "record_game_state", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
        "Also log the fighters, health and match events (the test harness's read-only probes).");
    ADD("debug", "auto_bundle_hitch_ms", RECOMP_SETTING_INT, 0, 0, 5000, NULL, "RECOMP_RECORD_AUTO_HITCH_MS",
        RECOMP_SETTING_RESTART,
        "A frame at least this long writes a bundle by itself (at most five a run, a minute apart). 0: only marks.");
    ADD("debug", "hang_seconds", RECOMP_SETTING_INT, 15, 0, 600, NULL, "RECOMP_RECORD_HANG_SECS",
        RECOMP_SETTING_RESTART, "No frame for this many seconds writes a hang bundle. 0: never.");
    ADD("debug", "keep_sessions", RECOMP_SETTING_INT, 20, 1, 500, NULL, NULL, RECOMP_SETTING_RESTART,
        "Sessions kept whole; older ones keep their bundles and events only.");
    ADD("debug", "record_guest_ram", RECOMP_SETTING_BOOL, 0, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
        "Crash and hang bundles also hold the game's 64 MB of memory, for offline inspection.");
#undef ADD
    return n;
}

/* The file stderr is going to, when the caller redirected it to one. */
static int stderr_file(char *out, size_t size)
{
#if defined(_WIN32)
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    char buf[MAX_PATH * 2];
    DWORD n;
    if (!h || h == INVALID_HANDLE_VALUE || GetFileType(h) != FILE_TYPE_DISK) return 0;
    n = GetFinalPathNameByHandleA(h, buf, sizeof buf, FILE_NAME_NORMALIZED);
    if (!n || n >= sizeof buf) return 0;
    snprintf(out, size, "%s", strncmp(buf, "\\\\?\\", 4) == 0 ? buf + 4 : buf);
    return 1;
#else
    struct stat st;
    if (fstat(STDERR_FILENO, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
#if defined(__APPLE__)
    {
        char buf[PATH_MAX];
        if (fcntl(STDERR_FILENO, F_GETPATH, buf) == -1) return 0;
        snprintf(out, size, "%s", buf);
        return 1;
    }
#else
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/fd/2", buf, sizeof buf - 1);
        if (n <= 0) return 0;
        buf[n] = 0;
        snprintf(out, size, "%s", buf);
        return 1;
    }
#endif
#endif
}

/* {"display.aspect":"4:3",...}: every setting as this run started with it. */
static char *settings_json(void)
{
    size_t i, count = recomp_settings_count(), cap = 256 + count * 128, len = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    len += (size_t)snprintf(out + len, cap - len, "{");
    for (i = 0; i < count && len + 200 < cap; i++) {
        RecompSetting *s = recomp_settings_at(i);
        char value[RECOMP_SETTING_TEXT_MAX], esc[RECOMP_SETTING_TEXT_MAX * 2];
        size_t j, o = 0;
        recomp_settings_format(s, s->value, value, sizeof value);
        if (s->type == RECOMP_SETTING_STRING)
            snprintf(value, sizeof value, "%s", s->value_text);
        for (j = 0; value[j] && o + 3 < sizeof esc; j++) {
            if (value[j] == '"' || value[j] == '\\') esc[o++] = '\\';
            esc[o++] = value[j];
        }
        esc[o] = 0;
        len += (size_t)snprintf(out + len, cap - len, "%s\"%s.%s\":\"%s\"", i ? "," : "", s->section, s->key, esc);
    }
    snprintf(out + len, cap - len, "}");
    return out;
}

void pc_debug_start(const char *log_path, int force)
{
    RecorderConfig cfg;
    char log[1024] = "", key[RECOMP_SETTING_TEXT_MAX], audio[1024] = "", build[160];
    const char *pcm = getenv("RECOMP_APU_PCM");
    int vk, record_audio;
    char *json;

    if (!force && !recomp_settings_get("debug", "record", 0))
        return;
    if (log_path && *log_path)
        snprintf(log, sizeof log, "%s", log_path);
    else
        stderr_file(log, sizeof log);
    recomp_settings_get_text("debug", "mark_key", key, sizeof key, "F9");
    vk = input_key_from_name(key);
    record_audio = recomp_settings_get("debug", "record_audio", 1);
    /* The probes are read-only (docs/09-testing-harness.md); they give bundles the fighters and the match. */
    if (recomp_settings_get("debug", "record_game_state", 1) && !getenv("RECOMP_TEST_OBSERVATIONS"))
        _putenv_s("RECOMP_TEST_OBSERVATIONS", "1");
#if defined(_WIN32)
    snprintf(build, sizeof build, "%s windows-x64", DEFJAM_VERSION);
#elif defined(__APPLE__)
    snprintf(build, sizeof build, "%s macos", DEFJAM_VERSION);
#else
    snprintf(build, sizeof build, "%s linux", DEFJAM_VERSION);
#endif
    json = settings_json();

    memset(&cfg, 0, sizeof cfg);
    cfg.enabled = 1;
    cfg.root = "logs";
    cfg.video_fps = recomp_settings_get("debug", "record_fps", 10);
    cfg.ring_seconds = recomp_settings_get("debug", "record_seconds", 30);
    cfg.thumb_height = 240;
    cfg.after_seconds = 3;
    cfg.hitch_ms = 50;
    cfg.auto_bundle_hitch_ms = recomp_settings_get("debug", "auto_bundle_hitch_ms", 0);
    cfg.hang_seconds = recomp_settings_get("debug", "hang_seconds", 15);
    cfg.keep_sessions = recomp_settings_get("debug", "keep_sessions", 20);
    cfg.guest_ram = recomp_settings_get("debug", "record_guest_ram", 0);
    cfg.guest_base = guest_base;
    cfg.guest_size = GUEST_RAM_BYTES;
    cfg.log_path = log[0] ? log : NULL;
    /* A caller that already captures the audio keeps its file; the recorder reads it. */
    cfg.audio_path = pcm && *pcm ? pcm : NULL;
    cfg.build = build;
    cfg.settings_json = json;
    cfg.mark_vk = vk > 0 ? vk : 0x78;   /* VK_F9 */
    cfg.mark_key_name = vk > 0 ? key : "F9";
#if !defined(_WIN32)
    cfg.stall_probe_ms = hitch_probe_ms();
    cfg.stall_probe = guest_turn_probe;
#endif
    if (!recorder_init(&cfg)) {
        free(json);
        return;
    }
    free(json);
    if (record_audio && !(pcm && *pcm)) {
        snprintf(audio, sizeof audio, "%s" HOST_SEP "audio.pcm", recorder_session_dir());
        _putenv_s("RECOMP_APU_PCM", audio);
        recorder_set_audio_path(audio);
    }
    if (!log[0])
        fprintf(stderr, "[RECORD] the log is not a file this program can find; bundles will not hold its end\n");
}
