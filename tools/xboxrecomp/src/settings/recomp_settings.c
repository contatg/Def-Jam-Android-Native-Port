/* Player-facing settings: one table, one file. See recomp_settings.h. */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include "recomp_settings.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION g_lock;
static int g_lock_ready;
static void lock_enter(void)
{
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
    EnterCriticalSection(&g_lock);
}
static void lock_leave(void) { LeaveCriticalSection(&g_lock); }
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static void lock_enter(void) { pthread_mutex_lock(&g_lock); }
static void lock_leave(void) { pthread_mutex_unlock(&g_lock); }
#endif

#define MAX_LINE     512
#define MAX_UNKNOWN  256

typedef struct UnknownLine {
    char section[64];
    char line[MAX_LINE];    /* "key = value", as read */
} UnknownLine;

static RecompSetting *g_table;
static size_t g_count;
static UnknownLine *g_unknown;
static size_t g_unknown_count;
static RecompSettingChanged g_changed;
static void *g_changed_user;
static void (*g_log)(const char *message);

static void report(const char *fmt, ...)
{
    char buf[MAX_LINE + 128];
    va_list ap;
    if (!g_log) return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    g_log(buf);
}

static int equal_nocase(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static char *trim(char *s)
{
    char *end;
    while (*s && isspace((unsigned char)*s)) s++;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = 0;
    return s;
}

/* Text to a value of the setting's type. Returns 0 if the text is not one. */
static int parse_value(const RecompSetting *s, const char *text, int *out)
{
    switch (s->type) {
    case RECOMP_SETTING_BOOL:
        if (equal_nocase(text, "true") || equal_nocase(text, "yes") ||
            equal_nocase(text, "on") || !strcmp(text, "1")) { *out = 1; return 1; }
        if (equal_nocase(text, "false") || equal_nocase(text, "no") ||
            equal_nocase(text, "off") || !strcmp(text, "0")) { *out = 0; return 1; }
        return 0;
    case RECOMP_SETTING_INT: {
        char *end;
        long v = strtol(text, &end, 10);
        if (end == text || *end) return 0;
        *out = v < s->min ? s->min : v > s->max ? s->max : (int)v;
        return 1;
    }
    case RECOMP_SETTING_ENUM: {
        int i;
        for (i = 0; s->choices && s->choices[i]; i++)
            if (equal_nocase(text, s->choices[i])) { *out = i; return 1; }
        return 0;
    }
    case RECOMP_SETTING_STRING:
        *out = 0;
        return strlen(text) < RECOMP_SETTING_TEXT_MAX;
    }
    return 0;
}

/* The text a string setting is reset to. */
static void default_text(RecompSetting *s)
{
    snprintf(s->stored_text, sizeof(s->stored_text), "%s", s->def_text ? s->def_text : "");
}

static int valid_value(const RecompSetting *s, int *value)
{
    int n = 0;
    switch (s->type) {
    case RECOMP_SETTING_BOOL:
        *value = *value ? 1 : 0;
        return 1;
    case RECOMP_SETTING_INT:
        if (*value < s->min) *value = s->min;
        if (*value > s->max) *value = s->max;
        return 1;
    case RECOMP_SETTING_ENUM:
        while (s->choices && s->choices[n]) n++;
        return *value >= 0 && *value < n;
    case RECOMP_SETTING_STRING:
        return 0;           /* text only: see recomp_settings_set_text */
    }
    return 0;
}

/* Apply the environment override, if the variable is set to a valid value. */
static void apply_env(RecompSetting *s)
{
    const char *e = s->env ? getenv(s->env) : NULL;
    int v;
    s->value = s->stored;
    memcpy(s->value_text, s->stored_text, sizeof(s->value_text));
    if (s->source == RECOMP_SETTING_FROM_ENV)
        s->source = RECOMP_SETTING_FROM_DEFAULT;
    if (!e || !*e) return;
    if (parse_value(s, e, &v)) {
        s->value = v;
        if (s->type == RECOMP_SETTING_STRING)
            snprintf(s->value_text, sizeof(s->value_text), "%s", e);
        s->source = RECOMP_SETTING_FROM_ENV;
    } else {
        report("settings: %s=%s is not a valid value for %s.%s; ignored",
               s->env, e, s->section, s->key);
    }
}

void recomp_settings_register(RecompSetting *table, size_t count)
{
    size_t i;
    g_table = table;
    g_count = count;
    for (i = 0; i < count; i++) {
        table[i].stored = table[i].def;
        default_text(&table[i]);
        table[i].source = RECOMP_SETTING_FROM_DEFAULT;
        apply_env(&table[i]);
    }
}

void recomp_settings_reset(void)
{
    g_table = NULL;
    g_count = 0;
    free(g_unknown);
    g_unknown = NULL;
    g_unknown_count = 0;
    g_changed = NULL;
    g_changed_user = NULL;
    g_log = NULL;
}

size_t recomp_settings_count(void) { return g_count; }
RecompSetting *recomp_settings_at(size_t index) { return index < g_count ? &g_table[index] : NULL; }

RecompSetting *recomp_settings_find(const char *section, const char *key)
{
    size_t i;
    if (!section || !key) return NULL;
    for (i = 0; i < g_count; i++)
        if (equal_nocase(g_table[i].section, section) && equal_nocase(g_table[i].key, key))
            return &g_table[i];
    return NULL;
}

int recomp_settings_get(const char *section, const char *key, int fallback)
{
    const RecompSetting *s = recomp_settings_find(section, key);
    return s ? s->value : fallback;
}

const char *recomp_settings_get_text(const char *section, const char *key,
                                     char *buf, size_t size, const char *fallback)
{
    const RecompSetting *s = recomp_settings_find(section, key);
    if (!buf || !size) return fallback;
    if (!s || s->type != RECOMP_SETTING_STRING) return fallback;
    lock_enter();
    snprintf(buf, size, "%s", s->value_text);
    lock_leave();
    return buf;
}

int recomp_settings_set(const char *section, const char *key, int value)
{
    RecompSetting *s = recomp_settings_find(section, key);
    int stored_changed, effective_changed, before;
    if (!s || !valid_value(s, &value)) return -1;
    lock_enter();
    before = s->value;
    stored_changed = s->stored != value;
    s->stored = value;
    if (s->source != RECOMP_SETTING_FROM_ENV) {
        s->value = value;
        s->source = RECOMP_SETTING_FROM_FILE;
    }
    effective_changed = s->value != before;
    lock_leave();
    if (effective_changed && g_changed)
        g_changed(s, g_changed_user);
    return stored_changed;
}

int recomp_settings_set_text(const char *section, const char *key, const char *text)
{
    RecompSetting *s = recomp_settings_find(section, key);
    int v;
    if (!s || !text) return -1;
    if (s->type == RECOMP_SETTING_STRING) {
        char copy[RECOMP_SETTING_TEXT_MAX], *t;
        int stored_changed, effective_changed;
        if (strlen(text) >= sizeof(copy)) return -1;
        snprintf(copy, sizeof(copy), "%s", text);
        t = trim(copy);
        lock_enter();
        stored_changed = strcmp(s->stored_text, t) != 0;
        effective_changed = 0;
        snprintf(s->stored_text, sizeof(s->stored_text), "%s", t);
        if (s->source != RECOMP_SETTING_FROM_ENV) {
            effective_changed = strcmp(s->value_text, t) != 0;
            snprintf(s->value_text, sizeof(s->value_text), "%s", t);
            s->source = RECOMP_SETTING_FROM_FILE;
        }
        lock_leave();
        if (effective_changed && g_changed)
            g_changed(s, g_changed_user);
        return stored_changed;
    }
    if (!parse_value(s, text, &v)) return -1;
    return recomp_settings_set(section, key, v);
}

const char *recomp_settings_format(const RecompSetting *s, int value, char *buf, size_t size)
{
    switch (s->type) {
    case RECOMP_SETTING_BOOL: snprintf(buf, size, "%s", value ? "true" : "false"); break;
    case RECOMP_SETTING_INT:  snprintf(buf, size, "%d", value); break;
    case RECOMP_SETTING_ENUM: {
        int n = 0;
        while (s->choices && s->choices[n]) n++;
        snprintf(buf, size, "%s", value >= 0 && value < n ? s->choices[value] : "");
        break;
    }
    case RECOMP_SETTING_STRING: snprintf(buf, size, "%s", s->stored_text); break;
    }
    return buf;
}

const char *recomp_settings_default_text(const RecompSetting *s, char *buf, size_t size)
{
    if (s->type == RECOMP_SETTING_STRING)
        snprintf(buf, size, "%s", s->def_text ? s->def_text : "");
    else
        recomp_settings_format(s, s->def, buf, size);
    return buf;
}

int recomp_settings_reset_to_default(const char *section, const char *key)
{
    const RecompSetting *s = recomp_settings_find(section, key);
    char text[RECOMP_SETTING_TEXT_MAX];
    if (!s) return -1;
    if (s->type == RECOMP_SETTING_STRING)
        return recomp_settings_set_text(section, key, recomp_settings_default_text(s, text, sizeof(text)));
    return recomp_settings_set(section, key, s->def);
}

void recomp_settings_on_change(RecompSettingChanged callback, void *user)
{
    g_changed = callback;
    g_changed_user = user;
}

void recomp_settings_set_log(void (*log)(const char *message)) { g_log = log; }

static void keep_unknown(const char *section, const char *line)
{
    UnknownLine *u;
    if (g_unknown_count >= MAX_UNKNOWN) return;
    if (!g_unknown) {
        g_unknown = (UnknownLine *)calloc(MAX_UNKNOWN, sizeof(UnknownLine));
        if (!g_unknown) return;
    }
    u = &g_unknown[g_unknown_count++];
    snprintf(u->section, sizeof(u->section), "%s", section);
    snprintf(u->line, sizeof(u->line), "%s", line);
}

int recomp_settings_load(const char *path)
{
    FILE *f;
    char raw[MAX_LINE], section[64] = "";
    int read = 0;
    size_t i;

    for (i = 0; i < g_count; i++) {
        g_table[i].stored = g_table[i].def;
        default_text(&g_table[i]);
        g_table[i].source = RECOMP_SETTING_FROM_DEFAULT;
    }
    g_unknown_count = 0;

    f = path ? fopen(path, "r") : NULL;
    if (!f) {
        FILE *probe;
        for (i = 0; i < g_count; i++) apply_env(&g_table[i]);
        if (!path) return 0;
        /* Missing is fine; present and unreadable is not. */
        probe = fopen(path, "rb");
        if (probe) { fclose(probe); return -1; }
        return 0;
    }

    while (fgets(raw, sizeof(raw), f)) {
        char *line = trim(raw), *eq, *key, *val;
        RecompSetting *s;
        int v;
        /* A UTF-8 byte-order mark, which Notepad may add. */
        if ((unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB &&
            (unsigned char)line[2] == 0xBF)
            line = trim(line + 3);
        if (!*line || *line == ';' || *line == '#') continue;
        if (*line == '[') {
            char *close = strchr(line, ']');
            if (close) {
                *close = 0;
                snprintf(section, sizeof(section), "%s", trim(line + 1));
            }
            continue;
        }
        eq = strchr(line, '=');
        if (!eq) continue;
        {
            char copy[MAX_LINE];
            snprintf(copy, sizeof(copy), "%s", line);
            *eq = 0;
            key = trim(line);
            val = trim(eq + 1);
            s = recomp_settings_find(section, key);
            if (!s) { keep_unknown(section, copy); continue; }
        }
        if (parse_value(s, val, &v)) {
            s->stored = v;
            if (s->type == RECOMP_SETTING_STRING)
                snprintf(s->stored_text, sizeof(s->stored_text), "%s", val);
            s->source = RECOMP_SETTING_FROM_FILE;
            read++;
        } else {
            report("settings: [%s] %s = %s is not valid; using the default",
                   section, key, val);
        }
    }
    fclose(f);
    for (i = 0; i < g_count; i++) apply_env(&g_table[i]);
    return read;
}

static void write_unknown(FILE *f, const char *section)
{
    size_t i;
    for (i = 0; i < g_unknown_count; i++)
        if (equal_nocase(g_unknown[i].section, section))
            fprintf(f, "%s\n", g_unknown[i].line);
}

static int section_is_known(const char *section)
{
    size_t i;
    for (i = 0; i < g_count; i++)
        if (equal_nocase(g_table[i].section, section)) return 1;
    return 0;
}

int recomp_settings_save(const char *path)
{
    char tmp[1024], text[RECOMP_SETTING_TEXT_MAX];
    FILE *f;
    size_t i, j;
    int ok;

    if (!path || snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return -1;
    f = fopen(tmp, "w");
    if (!f) return -1;

    lock_enter();
    for (i = 0; i < g_count; i++) {
        const RecompSetting *s = &g_table[i];
        int first = 1;
        for (j = 0; j < i; j++)
            if (equal_nocase(g_table[j].section, s->section)) { first = 0; break; }
        if (!first) continue;
        fprintf(f, "%s[%s]\n", i ? "\n" : "", s->section);
        for (j = i; j < g_count; j++) {
            const RecompSetting *t = &g_table[j];
            if (!equal_nocase(t->section, s->section)) continue;
            if (t->help) fprintf(f, "; %s\n", t->help);
            if (t->type == RECOMP_SETTING_ENUM && t->choices) {
                int k;
                fprintf(f, "; one of:");
                for (k = 0; t->choices[k]; k++) fprintf(f, " %s", t->choices[k]);
                fprintf(f, "\n");
            } else if (t->type == RECOMP_SETTING_INT) {
                fprintf(f, "; %d to %d\n", t->min, t->max);
            }
            if (t->flags & RECOMP_SETTING_RESTART)
                fprintf(f, "; takes effect at the next start\n");
            fprintf(f, "%s = %s\n", t->key,
                    recomp_settings_format(t, t->stored, text, sizeof(text)));
        }
        write_unknown(f, s->section);
    }
    /* Sections this table has no setting in at all. */
    for (i = 0; i < g_unknown_count; i++) {
        int first = 1;
        if (section_is_known(g_unknown[i].section)) continue;
        for (j = 0; j < i; j++)
            if (equal_nocase(g_unknown[j].section, g_unknown[i].section)) { first = 0; break; }
        if (!first) continue;
        fprintf(f, "\n[%s]\n", g_unknown[i].section);
        write_unknown(f, g_unknown[i].section);
    }
    lock_leave();

    ok = !ferror(f);
    if (fclose(f)) ok = 0;
    if (!ok) { remove(tmp); return -1; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { remove(tmp); return -1; }
#else
    if (rename(tmp, path)) { remove(tmp); return -1; }
#endif
    return 0;
}
