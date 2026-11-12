#define _CRT_SECURE_NO_WARNINGS
/* recomp_settings: defaults, file round trip, environment precedence, unknown
 * keys, bad values and the change callback. */

#include "recomp_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failed++; } } while (0)

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}

static const char *const k_aspect[] = { "4:3", "stretch", NULL };

static RecompSetting g_table[] = {
    { "display", "fullscreen", RECOMP_SETTING_BOOL, 0, 0, 0, NULL, NULL, 0, "Borderless fullscreen." },
    { "display", "render_scale", RECOMP_SETTING_INT, 2, 1, 4, NULL, "TEST_RENDER_SCALE",
      RECOMP_SETTING_RESTART, "Internal resolution multiplier." },
    { "display", "aspect", RECOMP_SETTING_ENUM, 0, 0, 0, k_aspect, NULL, 0, "Picture shape." },
    { "input", "rumble", RECOMP_SETTING_BOOL, 1, 0, 0, NULL, "TEST_RUMBLE", 0, NULL },
};
#define COUNT (sizeof(g_table) / sizeof(g_table[0]))

static int g_changes;
static char g_last_key[32];
static void changed(const RecompSetting *s, void *user)
{
    (void)user;
    g_changes++;
    snprintf(g_last_key, sizeof(g_last_key), "%s", s->key);
}

static int g_logged;
static void logger(const char *message) { (void)message; g_logged++; }

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static int file_contains(const char *path, const char *needle)
{
    char buf[4096];
    size_t n;
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    const char *path = "recomp_settings_test.ini";
    remove(path);
    set_env("TEST_RENDER_SCALE", NULL);
    set_env("TEST_RUMBLE", NULL);

    /* Defaults, and a missing file is not an error. */
    recomp_settings_register(g_table, COUNT);
    recomp_settings_set_log(logger);
    CHECK(recomp_settings_load(path) == 0);
    CHECK(recomp_settings_get("display", "fullscreen", 9) == 0);
    CHECK(recomp_settings_get("display", "render_scale", 9) == 2);
    CHECK(recomp_settings_get("display", "aspect", 9) == 0);
    CHECK(recomp_settings_get("input", "rumble", 9) == 1);
    CHECK(recomp_settings_get("display", "nothing", 9) == 9);
    CHECK(recomp_settings_find("DISPLAY", "Fullscreen") != NULL);

    /* Set, clamp, reject, callback. */
    recomp_settings_on_change(changed, NULL);
    CHECK(recomp_settings_set("display", "fullscreen", 1) == 1);
    CHECK(g_changes == 1 && !strcmp(g_last_key, "fullscreen"));
    CHECK(recomp_settings_set("display", "fullscreen", 1) == 0);
    CHECK(g_changes == 1);
    CHECK(recomp_settings_set("display", "render_scale", 99) == 1);
    CHECK(recomp_settings_get("display", "render_scale", 0) == 4);
    CHECK(recomp_settings_set("display", "aspect", 2) == -1);
    CHECK(recomp_settings_set_text("display", "aspect", "Stretch") == 1);
    CHECK(recomp_settings_get("display", "aspect", 0) == 1);
    CHECK(recomp_settings_set_text("display", "aspect", "21:9") == -1);
    CHECK(recomp_settings_set("nope", "nope", 1) == -1);

    /* Round trip. */
    CHECK(recomp_settings_save(path) == 0);
    CHECK(file_contains(path, "[display]"));
    CHECK(file_contains(path, "fullscreen = true"));
    CHECK(file_contains(path, "render_scale = 4"));
    CHECK(file_contains(path, "aspect = stretch"));
    CHECK(file_contains(path, "; takes effect at the next start"));
    recomp_settings_register(g_table, COUNT);
    CHECK(recomp_settings_get("display", "fullscreen", 9) == 0);
    CHECK(recomp_settings_load(path) == 4);
    CHECK(recomp_settings_get("display", "fullscreen", 9) == 1);
    CHECK(recomp_settings_get("display", "render_scale", 9) == 4);
    CHECK(recomp_settings_get("display", "aspect", 9) == 1);
    CHECK(recomp_settings_find("display", "aspect")->source == RECOMP_SETTING_FROM_FILE);

    /* The environment wins while set, and is never written to the file. */
    set_env("TEST_RENDER_SCALE", "1");
    CHECK(recomp_settings_load(path) == 4);
    CHECK(recomp_settings_get("display", "render_scale", 9) == 1);
    CHECK(recomp_settings_find("display", "render_scale")->source == RECOMP_SETTING_FROM_ENV);
    g_changes = 0;
    CHECK(recomp_settings_set("display", "render_scale", 3) == 1);
    CHECK(recomp_settings_get("display", "render_scale", 9) == 1);
    CHECK(g_changes == 0);
    CHECK(recomp_settings_save(path) == 0);
    CHECK(file_contains(path, "render_scale = 3"));
    set_env("TEST_RENDER_SCALE", NULL);
    CHECK(recomp_settings_load(path) == 4);
    CHECK(recomp_settings_get("display", "render_scale", 9) == 3);

    /* An invalid override is ignored and reported. */
    g_logged = 0;
    set_env("TEST_RUMBLE", "maybe");
    recomp_settings_load(path);
    CHECK(recomp_settings_get("input", "rumble", 9) == 1);
    CHECK(g_logged == 1);
    set_env("TEST_RUMBLE", NULL);

    /* Bad values fall back; unknown keys and sections survive a save; a
     * byte-order mark, comments and spacing are tolerated. */
    write_file(path,
        "\xEF\xBB\xBF; a comment\n"
        "[display]\n"
        "fullscreen=maybe\n"
        "  render_scale =  3  \n"
        "future_key = 7\n"
        "# another\n"
        "[mods]\n"
        "pack = hd\n");
    g_logged = 0;
    CHECK(recomp_settings_load(path) == 1);
    CHECK(g_logged == 1);
    CHECK(recomp_settings_get("display", "fullscreen", 9) == 0);
    CHECK(recomp_settings_get("display", "render_scale", 9) == 3);
    CHECK(recomp_settings_save(path) == 0);
    CHECK(file_contains(path, "future_key = 7"));
    CHECK(file_contains(path, "[mods]"));
    CHECK(file_contains(path, "pack = hd"));
    CHECK(file_contains(path, "fullscreen = false"));
    CHECK(recomp_settings_load(path) == 4);

    remove(path);
    recomp_settings_reset();
    CHECK(recomp_settings_count() == 0);

    /* Text settings: default, file round trip, environment precedence, a value
     * that does not fit, and the change callback. */
    {
        static RecompSetting text_table[] = {
            { "keyboard", "a", RECOMP_SETTING_STRING, 0, 0, 0, NULL, "TEST_KEY_A", 0,
              "Keys for the A button.", "L, Space" },
            { "keyboard", "b", RECOMP_SETTING_STRING, 0, 0, 0, NULL, NULL, 0, NULL, NULL },
        };
        char buf[RECOMP_SETTING_TEXT_MAX], toolong[RECOMP_SETTING_TEXT_MAX + 8];
        set_env("TEST_KEY_A", NULL);
        recomp_settings_register(text_table, 2);
        recomp_settings_set_log(logger);
        CHECK(recomp_settings_load(path) == 0);
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "L, Space"));
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "b", buf, sizeof(buf), "?"), ""));
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "nope", buf, sizeof(buf), "?"), "?"));
        /* An integer accessor does not apply to text. */
        CHECK(recomp_settings_set("keyboard", "a", 1) == -1);

        g_changes = 0;
        recomp_settings_on_change(changed, NULL);
        CHECK(recomp_settings_set_text("keyboard", "a", "  J , Mouse1 ") == 1);
        CHECK(g_changes == 1 && !strcmp(g_last_key, "a"));
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "J , Mouse1"));
        CHECK(recomp_settings_set_text("keyboard", "a", "J , Mouse1") == 0);
        CHECK(g_changes == 1);

        memset(toolong, 'x', sizeof(toolong) - 1);
        toolong[sizeof(toolong) - 1] = 0;
        CHECK(recomp_settings_set_text("keyboard", "a", toolong) == -1);
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "J , Mouse1"));

        CHECK(recomp_settings_save(path) == 0);
        CHECK(file_contains(path, "a = J , Mouse1"));
        CHECK(file_contains(path, "b = \n") || file_contains(path, "b =\n"));
        recomp_settings_set_text("keyboard", "a", "K");
        CHECK(recomp_settings_load(path) == 2);
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "J , Mouse1"));

        /* The environment wins and is not written. */
        set_env("TEST_KEY_A", "Q");
        recomp_settings_load(path);
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "Q"));
        recomp_settings_save(path);
        set_env("TEST_KEY_A", NULL);
        CHECK(file_contains(path, "a = J , Mouse1"));
        recomp_settings_load(path);
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "J , Mouse1"));

        /* Back to the default, text and notification included. */
        g_changes = 0;
        CHECK(recomp_settings_set_text("keyboard", "a", "Z") == 1);
        CHECK(recomp_settings_reset_to_default("keyboard", "a") == 1);
        CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "L, Space"));
        CHECK(recomp_settings_reset_to_default("keyboard", "a") == 0);
        CHECK(recomp_settings_reset_to_default("keyboard", "nope") == -1);
        CHECK(!strcmp(recomp_settings_default_text(recomp_settings_find("keyboard", "a"), buf, sizeof(buf)), "L, Space"));

        /* A file value too long for the setting falls back to the default. */
        {
            char line[RECOMP_SETTING_TEXT_MAX * 2 + 32];
            snprintf(line, sizeof(line), "[keyboard]\na = %s\n", toolong);
            write_file(path, line);
            g_logged = 0;
            recomp_settings_load(path);
            CHECK(g_logged == 1);
            CHECK(!strcmp(recomp_settings_get_text("keyboard", "a", buf, sizeof(buf), "?"), "L, Space"));
        }
        remove(path);
        recomp_settings_reset();
    }

    if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
    printf("recomp_settings: all checks passed\n");
    return 0;
}
