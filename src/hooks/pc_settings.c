/*
 * The port's player-facing settings and start-up paths.
 *
 * Until M6 the program only ran correctly from scripts/run.ps1: it needed four
 * environment variables, the repository root as its working directory and
 * DEFJAM_DATA for the saves. This file makes the executable start by itself,
 * and gives a player one file to edit instead of environment variables.
 *
 *   settings.ini   in the data folder (beside `extracted` and `save`), or
 *                  beside the executable when there is no data folder.
 *                  RECOMP_SETTINGS=<path> names another file;
 *                  RECOMP_SETTINGS=none uses the defaults and writes nothing,
 *                  which is what the test harness does so a player's choices
 *                  never change a regression run.
 *
 * Precedence is the settings library's: default, then the file, then the
 * setting's environment variable. Every RECOMP_* variable still works.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "recomp_settings.h"
#include "d3d8_present.h"
#if !defined(_WIN32)
#include "d3d8_vk.h"
#endif
#include "pc_settings.h"
#if defined(__ANDROID__)
#include "pc_language.h"
#endif
#include "pc_input.h"
#include "pc_ui.h"
#include "pc_debug.h"
#include "patches/widescreen.h"
#include "patches/picture.h"

/* Picture shape: the console's 4:3, stretched, or widescreen (docs/widescreen); the widescreen
 * choices are indices 2.. and are read once at start-up (src/patches/widescreen.c). */
static const char *const k_aspect[] = { "4:3", "stretch", "widescreen (auto)", "16:9", "16:10",
                                        "21:9 (experimental)", "32:9 (experimental)", NULL };
static const char *const k_filter[] = { "smooth", "sharp", NULL };
static const char *const k_ws_hud[] = { "auto", "edges", "centered", NULL };
static const char *const k_ws_cutscenes[] = { "authored", "fill", NULL };
static const char *const k_ws_fill[] = { "blur", "black", NULL };

/* A four-fighter match holds about 2 GiB of replacements. At 512 MiB the cache evicted and
 * re-decoded dozens of textures every two seconds in a fight on the Steam Deck. */
#define TEXTURE_CACHE_MB 2048
#if defined(__ANDROID__)
#define RENDER_DEFAULT145 1
#define VSYNC_DEFAULT145 0
#else
#define RENDER_DEFAULT145 2
#define VSYNC_DEFAULT145 1
#endif
static const char *const k_android_profile[] = {"auto","low-end","normal",NULL};
static RecompSetting g_display_settings[] = {
#if defined(__ANDROID__)
    {"graphics","preset145",RECOMP_SETTING_INT,0,0,3,NULL,NULL,0,"In-game graphics preset:0 custom,1 performance,2 balanced,3 quality. Does not change simulation."},
    {"graphics","fxaa145",RECOMP_SETTING_INT,-1,-1,1,NULL,NULL,0,"-1 inherits previous Android FXAA choice;0 off,1 on. Live in-game graphics menu."},
    {"graphics","hud145",RECOMP_SETTING_INT,-1,-1,1,NULL,NULL,0,"-1 inherits previous Android FPS counter choice;0 off,1 on."},
    {"graphics","hud_position145",RECOMP_SETTING_INT,0,0,2,NULL,NULL,0,"0 top-right,1 top-center,2 top-left."},
    {"graphics","precise_barriers144",RECOMP_SETTING_BOOL,1,0,0,NULL,"RECOMP_VK_BARRIERS144",RECOMP_SETTING_RESTART,"Layout-specific Vulkan barriers; conservative GPU mode retains broad dependencies."},
    {"graphics","fsr144",RECOMP_SETTING_INT,0,0,1,NULL,"RECOMP_VK_FSR144",RECOMP_SETTING_RESTART,"AMD FSR1 EASU+RCAS presentation upscaling. Optional GPU cost; no frame generation."},
    {"graphics","screen_filter144",RECOMP_SETTING_INT,0,0,5,NULL,"RECOMP_VK_FILTER144",RECOMP_SETTING_RESTART,"0 original,1 sharpen,2 vivid,3 scanlines,4 CRT,5 tone mapping (SDR)."},
    {"graphics","spatial34",RECOMP_SETTING_INT,0,0,2,NULL,"RECOMP_VK_SPATIAL34",RECOMP_SETTING_RESTART,"Presentation upscaler: 0 original, 1 Catmull-Rom bicubic, 2 Lanczos-2; independent of guest. Restart required."},
    {"graphics","anisotropy144",RECOMP_SETTING_INT,1,1,16,NULL,"RECOMP_VK_ANISO144",RECOMP_SETTING_RESTART,"Optional linear mipmapped texture filtering, limited to device support."},
    {"graphics","shader_preload144",RECOMP_SETTING_INT,1,0,1,NULL,"RECOMP_VK_PRELOAD144",RECOMP_SETTING_RESTART,"Bounded startup warm-up of prior-run shaders/pipelines; first run learns."},
    {"graphics","fsr_sharpness144",RECOMP_SETTING_INT,80,0,200,NULL,"RECOMP_VK_SHARPNESS144",RECOMP_SETTING_RESTART,"RCAS attenuation in hundredths of a stop:0 strongest,200 softer."},
    {"vksched","profile",RECOMP_SETTING_ENUM,0,0,0,k_android_profile,NULL,RECOMP_SETTING_RESTART,"Presentation preset. No guest simulation changes."},
    {"vksched","present_limit",RECOMP_SETTING_INT,-1,-1,60,NULL,NULL,RECOMP_SETTING_RESTART,"-1 auto/launcher,0 original,30/60 host ceiling."},
    {"vksched","menus_limit",RECOMP_SETTING_INT,60,0,120,NULL,NULL,RECOMP_SETTING_RESTART,"Auto profile: menu cap. Movies keep their authored cadence."},
    {"vksched","exclude_prime34",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Weak-phone preset: guest and native threads avoid the fastest CPU where supported."},
    {"vksched","kernel_log26",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"0: disable xbox_log before timestamp/format/locking and do not create xbox_kernel.log;1 restore diagnostic logger;restart required."},
    {"graphics","fg_engine39",RECOMP_SETTING_INT,0,0,2,NULL,NULL,RECOMP_SETTING_RESTART,"0 off, 1 Win-FG v37, 2 Lossless LSFG. Combat only."},
    {"graphics","framegen34",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Lossless Scaling 3.1 Performance frame generation; presentation only."},
    {"graphics","framegen27",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Optional native LSFG3.1 Performance x2/x3/x4 from user-imported Lossless shaders. Default OFF; adds video latency; no extra simulation steps."},
    {"graphics","lsfg32_height",RECOMP_SETTING_INT,480,240,960,NULL,"DEFJAM_LSFG32_HEIGHT",RECOMP_SETTING_RESTART,"Private LSFG snapshot height limit; preserves game render resolution. Default480."},
    {"graphics","lsfg30_multiplier",RECOMP_SETTING_INT,2,2,4,NULL,"DEFJAM_LSFG30_MULTIPLIER",RECOMP_SETTING_RESTART,"LSFG generated presentation multiplier:2/3/4. GPU and display can limit effective ratio."},
    {"graphics","lsfg30_refresh",RECOMP_SETTING_INT,60,60,120,NULL,"DEFJAM_LSFG30_REFRESH",RECOMP_SETTING_RESTART,"LSFG output ceiling:60/90/120Hz, if supported by the display."},
    {"graphics","lsfg27_flow",RECOMP_SETTING_INT,25,25,100,NULL,NULL,RECOMP_SETTING_RESTART,"LSFG optical-flow resolution percentage;25 low-cost default,50/100 optional. Does not change game resolution."},
    {"graphics","lsfg27_fp16",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Request FP16 LSFG shaders only if the Turnip device supports the feature; FP32 fallback. Restart required."},
    {"graphics","lsfg27_gpu_budget_us",RECOMP_SETTING_INT,4000,2000,8000,NULL,NULL,RECOMP_SETTING_RESTART,"Auto-disable framegen after12 GPU EWMA samples above this budget in microseconds; default4000."},
    {"graphics","lsfg27_budget_guard",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Pause FG when GPU budget is exceeded; 0 disables the budget guard."},
    {"graphics","lsfg27_warmup_samples",RECOMP_SETTING_INT,60,0,600,NULL,NULL,RECOMP_SETTING_RESTART,"Generated-frame GPU samples ignored after startup; default 60."},
    {"graphics","lsfg27_budget_retry_s",RECOMP_SETTING_INT,45,0,600,NULL,NULL,RECOMP_SETTING_RESTART,"Seconds before retry after GPU budget pause; 0 makes it permanent."},
    {"graphics","lsfg33_slow_guard",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Suspend FG below 20 fps; 0 allows low-source generation down to about 10 fps."},
    {"graphics","framegen26",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Experimental Vulkan image-based optical flow, combat only,60Hz presentation; adds20-60ms buffering; default OFF. Real FPS/simulation unchanged."},
    {"vksched","power25",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Power management and bounded1Hz observations."},
    {"vksched","adpf25",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Request Android performance hints; APIs may be unavailable; original simulation unchanged."},
    {"vksched","power_log25",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Monitor-thread CPU,temperature,cooling,headroom1Hz; no per-frame proc reads."},
    {"vksched","sustained25",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Request sustained performance when the device supports it; OS selects clocks."},
    {"vksched","display_rate25",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Request60Hz for60fps;120Hz for40fps when compatible display modes exist."},
    {"vksched","affinity25",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Opt-in fixed native placement:render6,helpers4/5,mixer2or3; original allowed mask respected."},
    {"vksched","guest_worker_affinity",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Experimental guest workers5,IRQ4; timing not validated; restart required."},
    {"vksched","eco_resolution25",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Opt-in low-end boot at480p,aniso1,FSRoff,aspect preserved."},
    {"vksched","dynamic_resolution25",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Headroom>=.85 for5s requests480p active region; requires direct presentation; original allocation retained."},
    {"vksched","thermal_cap40_25",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Opt-in40fps after480p and persistent heat; existing cadence guard remains; cannot meet17ms p95 at40fps."},
    {"vksched","present_mode25",RECOMP_SETTING_INT,0,0,2,NULL,NULL,RECOMP_SETTING_RESTART,"0 preserve selection,1 FIFO,2 MAILBOX if available; device decides image count."},
    {"vksched","swap_images25",RECOMP_SETTING_INT,0,0,5,NULL,NULL,RECOMP_SETTING_RESTART,"0 preserve previous minimum,3 or5 requested images, clamped to driver capabilities."},
    {"vksched","mixer_cpu25",RECOMP_SETTING_INT,3,2,3,NULL,NULL,RECOMP_SETTING_RESTART,"Native placement only, no audio buffer/algorithm change."},
    {"vksched","adaptive_pack24",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Bypass repeatedly missing packed-vertex buckets; exact validation on cache hits; original gather on bypass."},
    {"vksched","program_index24",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Fixed exact-key program index; rebuild only on program creation/replacement."},
    {"vksched","fine_scopes22",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Exclusive wall scopes; instrumentation overhead included."},
    {"vksched","resources22",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Opt-in per-present /proc lane counters; measurable file IO overhead."},
    {"vksched","telemetry22",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"One-second hardware telemetry on existing monitor thread."},
    {"vksched","fifo22",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Force FIFO, request three images; restart, compare on device."},
    {"vksched","pin_render22",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Opt-in render CPU6, helpers CPU5, only if in original allowed mask."},
    {"vksched","vertex_min_batch",RECOMP_SETTING_INT,512,96,2048,NULL,NULL,RECOMP_SETTING_RESTART,"96/128/256/512/1024/2048. Default512; measure target-device cost."},
    {"vksched","vertex_participants",RECOMP_SETTING_INT,4,0,8,NULL,NULL,RECOMP_SETTING_RESTART,"0/4: dispatcher plus three helpers; 1 serial; 2..8 total participants. Sleeping condition-variable waits."},
    {"vksched","native_clock_hz140",RECOMP_SETTING_INT,60,60,60,NULL,NULL,RECOMP_SETTING_RESTART,"Legacy key retained: original guest clock only; native120 experiment removed."},
    {"vksched","thermal_adaptive",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Legacy ignored key. No application thermal ceiling or thermal worker."},
    {"vksched","vertex_cache",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Bounded exact-input packed vertex cache; verifies raw bytes on every hit."},
    {"vksched","audio_buffer_ms",RECOMP_SETTING_INT,60,43,85,NULL,NULL,RECOMP_SETTING_RESTART,"Bounded SDL audio preroll. More buffer adds output latency, not missing samples."},
    {"vksched","diagnostics",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Two-second frame windows; background reports every ten seconds."},
    {"vksched","path_cache",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Read-only disc path/miss cache; writable saves excluded. Restart after importing files."},
    {"vksched","hitch_details",RECOMP_SETTING_BOOL,1,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Bounded per-frame scope attribution; summaries every ten seconds."},
    {"vksched","guest_samples",RECOMP_SETTING_BOOL,0,0,0,NULL,NULL,RECOMP_SETTING_RESTART,"Opt-in 100Hz guest-thread CPU sampling. Profiling has overhead."},
#endif
    { "textures", "enabled", RECOMP_SETTING_BOOL, 0, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
      "Use texture packs from the data folder's mods directory." },
    { "textures", "packs", RECOMP_SETTING_STRING, 0, 0, 0, NULL, "RECOMP_TEXTURE_PACKS", RECOMP_SETTING_RESTART,
      "Pack folder names separated by semicolons. Later packs take precedence.", "faithful-hd" },
    { "textures", "cache_mb", RECOMP_SETTING_INT, TEXTURE_CACHE_MB, 32, 4096, NULL, "RECOMP_TEXTURE_CACHE_MB", RECOMP_SETTING_RESTART,
      "Memory budget for replacement textures in MiB, including their retained upload data." },
    { "textures", "dump", RECOMP_SETTING_BOOL, 0, 0, 0, NULL, NULL, RECOMP_SETTING_RESTART,
      "Capture source textures for making a pack, in hd-work/runtime under the data folder." },
    { "textures", "dump_limit", RECOMP_SETTING_INT, 2000, 0, 65536, NULL, "RECOMP_TEXTURE_DUMP_LIMIT", RECOMP_SETTING_RESTART,
      "Maximum new source images captured in one run." },
    { "display", "fullscreen", RECOMP_SETTING_BOOL, 0, 0, 0, NULL, NULL, 0,
      "Borderless full screen. Alt+Enter or F11 switches while playing." },
    { "display", "window_width", RECOMP_SETTING_INT, 1920, 320, 16384, NULL, NULL, 0,
      "Window size when not full screen. Resizing the window updates these." },
    { "display", "window_height", RECOMP_SETTING_INT, 1080, 240, 16384, NULL, NULL, 0, NULL },
    { "display", "aspect", RECOMP_SETTING_ENUM, 2, 0, 0, k_aspect, "RECOMP_ASPECT", 0,
      "4:3 keeps the console's picture shape with black bars; stretch fills the window; the widescreen shapes "
      "(beta) widen the 3D view and keep menus, HUD and movies in shape, and start with the game." },
    { "display", "widescreen_hud", RECOMP_SETTING_ENUM, 0, 0, 0, k_ws_hud, NULL, RECOMP_SETTING_RESTART,
      "Where the fight HUD goes in widescreen: the screen edges, or a centred 16:9 box (auto: edges up to 16:9)." },
    { "display", "widescreen_cutscenes", RECOMP_SETTING_ENUM, 0, 0, 0, k_ws_cutscenes, NULL, RECOMP_SETTING_RESTART,
      "Story cutscenes in widescreen: authored keeps the game's own 16:9 framing, fill always fills the screen." },
    { "display", "widescreen_side_fill", RECOMP_SETTING_ENUM, 0, 0, 0, k_ws_fill, NULL, RECOMP_SETTING_RESTART,
      "Beside 4:3 pictures (movies, menu backgrounds) in widescreen: a soft blurred extension, or black." },
    { "display", "widescreen_fov_cap", RECOMP_SETTING_INT, 150, 90, 170, NULL, NULL, RECOMP_SETTING_RESTART,
      "Widest horizontal field of view widescreen may use, in degrees (only very wide screens reach it)." },
#if defined(__ANDROID__)
    { "display", "render_height151", RECOMP_SETTING_INT, 0, 0, 2160, NULL, NULL,
      RECOMP_SETTING_RESTART, "Internal vertical resolution:480,540,720,960,1080,1440,1920,2160. 0 keeps legacy render_scale. Width follows aspect. Restart required." },
#endif
    { "display", "render_scale", RECOMP_SETTING_INT, RENDER_DEFAULT145, 1, 4, NULL, "RECOMP_RENDER_SCALE",
      RECOMP_SETTING_RESTART,
      "Internal render height: 1x = 480p, 2x = 960p, 3x = 1440p, 4x = 1920p. "
      "1080p output: 2x balances speed and clarity; 3x is sharper. 1440p output: 3x. "
      "Width follows Picture shape; window size is separate. Higher scales cost GPU time. "
      "Applies on Play/next launch." },
    { "display", "filter", RECOMP_SETTING_ENUM, 0, 0, 0, k_filter, NULL, 0,
      "How the picture is scaled to the window." },
    { "display", "vsync", RECOMP_SETTING_BOOL, VSYNC_DEFAULT145, 0, 0, NULL, "RECOMP_PRESENT_VSYNC", 0,
      "Pace frames on the display for even motion. Used when the refresh rate is a multiple of 60 (60, 120, 180, 240 Hz); other displays use the game's own 60 fps timer." },
    { "display", "gamma", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, "RECOMP_GAMMA",
      RECOMP_SETTING_RESTART,
      "Apply the game's own brightness curve, as the console does." },
    { "display", "brightness", RECOMP_SETTING_INT, 100, 50, 200, NULL, "RECOMP_BRIGHTNESS", 0,
      "Brightness of the picture's mid-tones; black and white stay as they are. 100 is the console's picture, "
      "higher is brighter." },
    { "display", "vignette", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, "RECOMP_VIGNETTE", 0,
      "The darkened screen edges that venues and scenes draw. Off removes them." },
    { "display", "vignette_strength", RECOMP_SETTING_INT, 100, 0, 100, NULL, "RECOMP_VIGNETTE_STRENGTH", 0,
      "How dark the vignette is, in percent of the game's own (100)." },
};
#define DISPLAY_SETTING_COUNT (sizeof(g_display_settings) / sizeof(g_display_settings[0]))

/* The display table followed by the input tables (pc_input.c): one array, one
 * registration, one file. */
static RecompSetting g_settings[DISPLAY_SETTING_COUNT + 64 + 16 + 16 + 4];
static size_t g_setting_count;

static char g_path[MAX_PATH * 4];   /* empty: do not load or save */
static char g_log_name[MAX_PATH * 2];   /* the log this program opened for itself, if it did */
static void (*g_fullscreen_notify)(int fullscreen);
static void (*g_window_size_notify)(int width, int height);

static void settings_log(const char *message)
{
    fprintf(stderr, "[SETTINGS] %s\n", message);
}

#if defined(_WIN32)

static int file_exists(const char *path)
{
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void strip_last(char *path)
{
    char *slash = strrchr(path, '\\');
    if (slash) *slash = 0;
}

/* Find the folder that holds `game\default.xbe` and make it current: here,
 * else beside the executable, else up to four levels above it (the build tree
 * is build\<preset>\ under the repository root). */
static int find_game_root(void)
{
    char dir[MAX_PATH], probe[MAX_PATH + 32];
    int up;

    if (file_exists("game\\default.xbe"))
        return 1;
    if (!GetModuleFileNameA(NULL, dir, sizeof(dir)))
        return 0;
    strip_last(dir);
    for (up = 0; up <= 4 && strchr(dir, '\\'); up++) {
        snprintf(probe, sizeof(probe), "%s\\game\\default.xbe", dir);
        if (file_exists(probe))
            return SetCurrentDirectoryA(dir) != 0;
        strip_last(dir);
    }
    return 0;
}

/* The data folder: DEFJAM_DATA, or the parent of the dump when the dump is a
 * folder named `extracted` (the layout docs/build-and-play.md sets up). Sets
 * DEFJAM_DATA when it was derived, so the save path in main.c follows. */
static int find_data_root(char *root, size_t size)
{
    const char *env = getenv("DEFJAM_DATA");
    char real[MAX_PATH];
    HANDLE h;
    DWORD n;
    const char *p;
    char *name;

    if (env && *env) {
        snprintf(root, size, "%s", env);
        return 1;
    }
    /* `game` is usually a junction; ask where it really is. */
    h = CreateFileA("game", 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    n = GetFinalPathNameByHandleA(h, real, sizeof(real), FILE_NAME_NORMALIZED);
    CloseHandle(h);
    if (!n || n >= sizeof(real))
        return 0;
    p = strncmp(real, "\\\\?\\", 4) == 0 ? real + 4 : real;
    snprintf(root, size, "%s", p);
    name = strrchr(root, '\\');
    if (!name || _stricmp(name + 1, "extracted") != 0)
        return 0;
    *name = 0;
    _putenv_s("DEFJAM_DATA", root);
    return 1;
}

/* Started by a double click there is nothing to print to: keep a log, as
 * scripts/run.ps1 does. A caller that redirected the output keeps its own. */
static void open_log_if_needed(void)
{
    HANDLE out = GetStdHandle(STD_ERROR_HANDLE);
    char name[MAX_PATH];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);

    if (out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN)
        return;
    CreateDirectoryA("logs", NULL);
    strftime(name, sizeof(name), "logs\\run-%Y%m%d-%H%M%S.log", t);
    freopen(name, "w", stdout);
    strcat(name, ".err");
    freopen(name, "w", stderr);
    snprintf(g_log_name, sizeof g_log_name, "%s", name);
    /* Buffered, as main.c sets the streams it was given. */
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
}

static int program_dir(char *out, size_t size)
{
    if (!GetModuleFileNameA(NULL, out, (DWORD)size))
        return 0;
    strip_last(out);
    return 1;
}

#else

#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void strip_last(char *path)
{
    char *slash = strrchr(path, '/');
    if (slash) *slash = 0;
}

/* The running program's own path, symbolic links resolved. */
static int program_path(char *out, size_t size)
{
    char raw[PATH_MAX], real[PATH_MAX];
#if defined(__APPLE__)
    uint32_t n = sizeof(raw);
    if (_NSGetExecutablePath(raw, &n))
        return 0;
#else
    ssize_t n = readlink("/proc/self/exe", raw, sizeof(raw) - 1);
    if (n <= 0)
        return 0;
    raw[n] = 0;
#endif
    if (!realpath(raw, real))
        return 0;
    snprintf(out, size, "%s", real);
    return 1;
}

/* As on Windows: here, else beside the program, else up to four levels above
 * it (the build tree is build/<preset>/ under the repository root). */
static int find_game_root(void)
{
    char dir[PATH_MAX], probe[PATH_MAX + 32];
    int up;

    if (file_exists("game/default.xbe"))
        return 1;
    if (!program_path(dir, sizeof(dir)))
        return 0;
    strip_last(dir);
    for (up = 0; up <= 4 && dir[0]; up++) {
        snprintf(probe, sizeof(probe), "%s/game/default.xbe", dir);
        if (file_exists(probe))
            return chdir(dir) == 0;
        strip_last(dir);
    }
    return 0;
}

/* DEFJAM_DATA, or the parent of the dump when `game` is a symbolic link to a
 * folder named `extracted`. */
static int find_data_root(char *root, size_t size)
{
    const char *env = getenv("DEFJAM_DATA");
    char real[PATH_MAX];
    char *name;

    if (env && *env) {
        snprintf(root, size, "%s", env);
        return 1;
    }
    if (!realpath("game", real))
        return 0;
    snprintf(root, size, "%s", real);
    name = strrchr(root, '/');
    if (!name || strcasecmp(name + 1, "extracted") != 0)
        return 0;
    *name = 0;
    setenv("DEFJAM_DATA", root, 1);
    return 1;
}

/* Started from a file manager there is no terminal: keep a log then. */
static void open_log_if_needed(void)
{
    char name[PATH_MAX];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    struct stat st;

    /* A terminal, a pipe or a file the caller chose all keep the output;
     * only /dev/null, which is what a launcher hands a program, loses it. */
    if (fstat(STDERR_FILENO, &st) == 0
            && !(S_ISCHR(st.st_mode) && !isatty(STDERR_FILENO)))
        return;
    mkdir("logs", 0755);
    strftime(name, sizeof(name), "logs/run-%Y%m%d-%H%M%S.log", t);
    freopen(name, "w", stdout);
    strcat(name, ".err");
    freopen(name, "w", stderr);
    snprintf(g_log_name, sizeof g_log_name, "%s", name);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    setvbuf(stderr, NULL, _IOFBF, 1 << 16);
}

static int program_dir(char *out, size_t size)
{
    if (!program_path(out, size))
        return 0;
    strip_last(out);
    return 1;
}

#endif

/* What scripts/run.ps1 and the harness have always set: the vertical-blank
 * interrupt, the push-buffer executor, its Direct3D 11 output and the USB
 * model. Each is "on if set", so a diagnostic run that needs one off sets
 * RECOMP_NO_DEFAULTS=1 and names the ones it wants. */
static void supply_runtime_defaults(void)
{
    static const char *const names[] = {
        "RECOMP_VBLANK", "RECOMP_PB_EXEC", "RECOMP_PB_D3D11", "RECOMP_USB" };
    size_t i;
    if (getenv("RECOMP_NO_DEFAULTS"))
        return;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!getenv(names[i]))
            _putenv_s(names[i], "1");
    /* The 64 MiB contiguous window, where this title keeps textures and vertex
     * data, is a bump allocator that never frees unless reclaim is on. A long
     * session ran it dry (576 "[CONTIG] arena exhausted" lines across a match
     * load, then UI corruption until restart; issue #26). RECOMP_NO_HEAP_RECLAIM=1
     * restores the old behaviour for comparison. */
    if (!getenv("RECOMP_HEAP_RECLAIM") && !getenv("RECOMP_NO_HEAP_RECLAIM"))
        _putenv_s("RECOMP_HEAP_RECLAIM", "1");
}

int pc_display(const char *key, int fallback)
{
    return recomp_settings_get("display", key, fallback);
}

void pc_settings_apply_display(void)
{
    if (ws_aspect() > 0.0f)       /* widescreen: the canvas's shape, with bars if the window differs */
        d3d8_present_set_aspect(1, (unsigned)(ws_aspect() * 10000.0f + 0.5f), 10000);
    else
        d3d8_present_set_aspect(pc_display("aspect", 0) != 1, 4, 3);   /* only "stretch" fills the window */
    d3d8_present_set_linear_filter(pc_display("filter", 0) == 0);
    d3d8_present_set_vsync(pc_display("vsync", 1));
    picture_apply_settings();
}

static void setting_changed(const RecompSetting *s, void *user)
{
    (void)user;
    pc_settings_apply_display();
    pc_input_apply();
    pc_ui_apply_settings();
    if (!strcmp(s->section, "display") && !strcmp(s->key, "fullscreen") && g_fullscreen_notify)
        g_fullscreen_notify(s->value);
    if (!strcmp(s->section, "display") && (!strcmp(s->key, "window_width") || !strcmp(s->key, "window_height")) &&
        g_window_size_notify)
        g_window_size_notify(pc_display("window_width", 1280), pc_display("window_height", 960));
}

void pc_settings_on_fullscreen(void (*notify)(int fullscreen))
{
    g_fullscreen_notify = notify;
}

void pc_settings_on_window_size(void (*notify)(int width, int height))
{
    g_window_size_notify = notify;
}

const char *pc_settings_path(void) { return g_path; }

void pc_settings_save(void)
{
    if (g_path[0] && recomp_settings_save(g_path))
        fprintf(stderr, "[SETTINGS] could not write %s\n", g_path);
}

void pc_display_set(const char *key, int value)
{
    if (recomp_settings_set("display", key, value) > 0 && g_path[0]) {
        if (recomp_settings_save(g_path))
            fprintf(stderr, "[SETTINGS] could not write %s\n", g_path);
    }
}

/* The launcher shows on every launch unless the player turned it off (docs/launcher in
 * pc_ui.h has the exact rule). It edits the settings, so the input layer is started again
 * afterwards and anything read once at start-up is read after it. */
#if !defined(_WIN32)
int host_posix_have_video(void);       /* src/host_posix.c */
int host_posix_shift_held(void);

static const char *const *g_argv;
static int g_argc;

void pc_settings_set_args(int argc, char **argv)
{
    g_argc = argc;
    g_argv = (const char *const *)argv;
}

static int has_arg(const char *flag)
{
    int i;
    for (i = 1; i < g_argc; i++)
        if (g_argv[i] && !strcmp(g_argv[i], flag))
            return 1;
    return 0;
}
#endif

static void run_launcher_if_wanted(void)
{
#if defined(_WIN32)
    const char *settings = getenv("RECOMP_SETTINGS");
    const char *cmd = GetCommandLineA();
    int test_run = (settings && !_stricmp(settings, "none")) ||
                   (getenv("RECOMP_PAD_SCRIPT") && !getenv("RECOMP_PAD_HOST"));
    int force = cmd && strstr(cmd, "--launcher") != NULL;
    int no = cmd && strstr(cmd, "--no-launcher") != NULL;
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

    if (!pc_launcher_wanted(recomp_settings_get("launcher", "skip", 0), force && !no, no, shift, test_run))
        return;
    if (!pc_launcher_run()) {
        fprintf(stderr, "[UI] the launcher was closed; not starting the game\n");
        fflush(stderr);
        ExitProcess(0);
    }
    pc_input_restart();
#else
    const char *settings = getenv("RECOMP_SETTINGS");
    int test_run = (settings && !strcasecmp(settings, "none")) ||
                   (getenv("RECOMP_PAD_SCRIPT") && !getenv("RECOMP_PAD_HOST")) ||
                   getenv("RECOMP_HEADLESS") != NULL;
    int force = has_arg("--launcher");
    int no = has_arg("--no-launcher");

    /* Nothing to show it in without a display; the game itself then runs off screen. */
    if (!host_posix_have_video())
        return;
    if (!pc_launcher_wanted(recomp_settings_get("launcher", "skip", 0), force && !no, no,
                            host_posix_shift_held(), test_run))
        return;
    if (!pc_launcher_run()) {
        fprintf(stderr, "[UI] the launcher was closed; not starting the game\n");
        fflush(stderr);
        pc_input_stop();
        fflush(stdout);
        _Exit(0);
    }
    pc_input_restart();
#endif
}

int pc_settings_init(void)
{
    char root[MAX_PATH * 2];
    const char *env = getenv("RECOMP_SETTINGS");
    int have_game = find_game_root();
    int have_root, read;

    if (have_game)
        open_log_if_needed();
    if (!have_game) {
        host_fatal(
            "The game files were not found.\n\n"
            "Create a folder, junction or symbolic link named 'game' beside this program\n"
            "(or in the repository root) that holds your own extracted dump, with\n"
            "default.xbe in it. See docs/build-and-play.md.");
        return 0;
    }
    supply_runtime_defaults();
    have_root = find_data_root(root, sizeof(root));

    if (env && !_stricmp(env, "none")) {
        g_path[0] = 0;
    } else if (env && *env) {
        snprintf(g_path, sizeof(g_path), "%s", env);
    } else if (have_root) {
        snprintf(g_path, sizeof(g_path), "%s" HOST_SEP "settings.ini", root);
    } else if (program_dir(g_path, MAX_PATH)) {
        strcat(g_path, HOST_SEP "settings.ini");
    }

    memcpy(g_settings, g_display_settings, sizeof(g_display_settings));
    g_setting_count = DISPLAY_SETTING_COUNT +
        pc_input_settings(g_settings + DISPLAY_SETTING_COUNT, 64);
    g_setting_count += pc_ui_settings(g_settings + g_setting_count, 16);
    g_setting_count += pc_debug_settings(g_settings + g_setting_count, 16);
#if defined(__ANDROID__)
    g_setting_count += pc_language_settings(g_settings + g_setting_count, 4);
#endif
    recomp_settings_register(g_settings, g_setting_count);
    recomp_settings_set_log(settings_log);
    read = g_path[0] ? recomp_settings_load(g_path) : 0;
    if (!g_path[0])
        fprintf(stderr, "[SETTINGS] defaults only (RECOMP_SETTINGS=none)\n");
    else if (read < 0)
        fprintf(stderr, "[SETTINGS] could not read %s; using defaults\n", g_path);
    else {
        fprintf(stderr, "[SETTINGS] %s (%d value(s) read)\n", g_path, read);
        /* Write it out on first run, and after an upgrade adds a setting, so
         * there is always a complete, commented file to edit. */
        if (read < (int)g_setting_count && recomp_settings_save(g_path))
            fprintf(stderr, "[SETTINGS] could not write %s\n", g_path);
    }
    if (have_root)
        fprintf(stderr, "[SETTINGS] data folder %s\n", root);
    else
        fprintf(stderr, "[SETTINGS] no data folder found; saves use the runtime's default location\n");

#if defined(__ANDROID__)
    extern void defjam_sched_settings_ready(void);
    defjam_sched_settings_ready();
#endif
    pc_input_start();
    run_launcher_if_wanted();

    /* Settings read once at start-up by code that looks at the environment (after the launcher,
     * which may have changed them). */
    if (!getenv("RECOMP_GAMMA") && !pc_display("gamma", 1))
        _putenv_s("RECOMP_GAMMA", "0");

    {
        char value[MAX_PATH*2], packs[RECOMP_SETTING_TEXT_MAX];
        if (!getenv("RECOMP_TEXTURE_PACKS")) {
            recomp_settings_get_text("textures","packs",packs,sizeof packs,"faithful-hd");
            _putenv_s("RECOMP_TEXTURE_PACKS",recomp_settings_get("textures","enabled",0) ? packs : "");
        }
        if (!getenv("RECOMP_TEXTURE_CACHE_MB")) {
            snprintf(value,sizeof value,"%d",recomp_settings_get("textures","cache_mb",TEXTURE_CACHE_MB));
            _putenv_s("RECOMP_TEXTURE_CACHE_MB",value);
        }
        if (!getenv("RECOMP_TEXTURE_DUMP_LIMIT")) {
            snprintf(value,sizeof value,"%d",recomp_settings_get("textures","dump_limit",2000));
            _putenv_s("RECOMP_TEXTURE_DUMP_LIMIT",value);
        }
        if (!getenv("RECOMP_TEXTURE_DUMP_DIR") && have_root && recomp_settings_get("textures","dump",0)) {
            snprintf(value,sizeof value,"%s/hd-work/runtime",root);
            _putenv_s("RECOMP_TEXTURE_DUMP_DIR",value);
        }
    }

#if defined(__ANDROID__)
    /* Prepare translated copies before the guest opens game files. */
    pc_language_start(have_root ? root : NULL, "game");
    fprintf(stderr, "[LANG40] story choice=%d effective=%d (0 auto, 1 all, 2 original)\n",
            recomp_settings_get("language", "story_subtitles", 1), pc_language_all_subtitles());

#endif

    /* The play-test recorder ([debug] record): after the launcher, which can turn it on,
     * and before the audio starts, whose capture it points into the session. */
#if defined(_WIN32)
    pc_debug_start(g_log_name[0] ? g_log_name : NULL,
                   GetCommandLineA() && strstr(GetCommandLineA(), "--record") != NULL);
#else
    pc_debug_start(g_log_name[0] ? g_log_name : NULL, has_arg("--record"));
#endif
    /* [debug] unlock_all: src/patches/dev_unlock.c reads the variable once the game runs. */
    if (!getenv("RECOMP_UNLOCK_ALL") && recomp_settings_get("debug", "unlock_all", 0))
        _putenv_s("RECOMP_UNLOCK_ALL", "1");

    d3d8_present_enable_scaling(1);
    ws_init(pc_display("window_width", 1280), pc_display("window_height", 960), pc_display("fullscreen", 0));
#if defined(_WIN32)
    d3d8_present_set_overlay(pc_ui_overlay, NULL);
#else
    pc_ui_register_overlay();
    /* Compiled shaders and the driver's pipeline cache are kept in
     * <data>/cache/vulkan, so a shader met in an earlier run is not compiled
     * again at its first draw. */
    if (have_root) {
        char cache[PATH_MAX];
        snprintf(cache, sizeof(cache), "%s/cache/vulkan", root);
        d3d8_vk_set_cache_dir(cache);
    }
#endif
    d3d8_present_set_render_scale((unsigned)pc_display("render_scale", 2));
#if defined(__ANDROID__)
    extern void d3d8_present_set_render_height151(unsigned height);
    d3d8_present_set_render_height151((unsigned)pc_display("render_height151",0));
#endif
    pc_settings_apply_display();
    recomp_settings_on_change(setting_changed, NULL);
    fflush(stderr);
    return 1;
}
