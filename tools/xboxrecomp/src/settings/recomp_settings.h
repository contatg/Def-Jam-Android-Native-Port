/*
 * Player-facing settings: one table, one file.
 *
 * A host program declares its settings as a table of RecompSetting and
 * registers it once. The table is the single source every front end goes
 * through -- the settings file, hotkeys, an overlay, a launcher -- so a value
 * changed in one place is the value everywhere.
 *
 * Precedence, lowest first: the table's default, the settings file, the
 * setting's environment variable. An environment variable is a per-run
 * override for scripts and diagnostics: it wins while it is set and is never
 * written to the file.
 *
 * The file is INI: "[section]" headers, "key = value" lines, and comments
 * starting with ';' or '#'. Keys this table does not know are kept and written
 * back, so a file shared between versions of a program loses nothing.
 *
 * Portable C with no dependencies. Not thread-safe for registration, loading
 * or saving; recomp_settings_get is safe to call from any thread once the
 * table is loaded, and recomp_settings_set takes an internal lock.
 */

#ifndef RECOMP_SETTINGS_H
#define RECOMP_SETTINGS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum RecompSettingType {
    RECOMP_SETTING_BOOL,   /* "true"/"false" (also 1/0, yes/no, on/off) */
    RECOMP_SETTING_INT,    /* clamped to [min, max] */
    RECOMP_SETTING_ENUM,   /* one of choices[]; the value is the index */
    RECOMP_SETTING_STRING  /* free text of at most RECOMP_SETTING_TEXT_MAX-1 characters */
} RecompSettingType;

/* Room for a string setting's text, terminator included. */
#define RECOMP_SETTING_TEXT_MAX 96

/* The change takes effect at the next start, not while running. */
#define RECOMP_SETTING_RESTART  0x1

typedef enum RecompSettingSource {
    RECOMP_SETTING_FROM_DEFAULT,
    RECOMP_SETTING_FROM_FILE,
    RECOMP_SETTING_FROM_ENV
} RecompSettingSource;

typedef struct RecompSetting {
    const char *section;            /* "display" */
    const char *key;                /* "fullscreen" */
    RecompSettingType type;
    int def;
    int min, max;                   /* RECOMP_SETTING_INT only */
    const char *const *choices;     /* RECOMP_SETTING_ENUM: NULL-terminated */
    const char *env;                /* overriding environment variable, or NULL */
    unsigned flags;
    const char *help;               /* one line, written above the key */
    const char *def_text;           /* RECOMP_SETTING_STRING: the default; NULL is "" */

    /* Managed by the library; leave zero in the initialiser. */
    int stored;                     /* default or file value: what gets saved */
    int value;                      /* effective value: stored, or the env override */
    RecompSettingSource source;
    char stored_text[RECOMP_SETTING_TEXT_MAX];  /* strings: what gets saved */
    char value_text[RECOMP_SETTING_TEXT_MAX];   /* strings: effective text */
} RecompSetting;

typedef void (*RecompSettingChanged)(const RecompSetting *setting, void *user);

/* Register the table. The array must outlive the library's use of it. Every
 * entry starts at its default; environment overrides are applied here and
 * again by recomp_settings_load. */
void recomp_settings_register(RecompSetting *table, size_t count);

/* Read a file over the defaults. A missing file is not an error (returns 0 and
 * leaves the defaults). Returns the number of known keys read, or -1 if the
 * file exists and cannot be read. Malformed and out-of-range values fall back
 * to the default and are reported through recomp_settings_set_log. */
int recomp_settings_load(const char *path);

/* Write every setting's stored value, with its help line, plus any unknown
 * keys read by the last load. Writes to a temporary file and renames it over
 * the target, so an interrupted save leaves the old file. Returns 0 on
 * success. */
int recomp_settings_save(const char *path);

RecompSetting *recomp_settings_find(const char *section, const char *key);
size_t recomp_settings_count(void);
RecompSetting *recomp_settings_at(size_t index);

/* The effective value. Returns `fallback` for an unknown setting. */
int recomp_settings_get(const char *section, const char *key, int fallback);

/* A string setting's effective text, copied into `buf`. Returns `buf`, or
 * `fallback` for an unknown setting or one that is not a string. Safe from any
 * thread. */
const char *recomp_settings_get_text(const char *section, const char *key,
                                     char *buf, size_t size, const char *fallback);

/* Set the stored value (clamped, or rejected for an enum index out of range).
 * The effective value follows unless an environment variable overrides it.
 * Calls the change callback when the effective value changed. Returns 1 if the
 * stored value changed, 0 if not, -1 for an unknown setting or a bad value. */
int recomp_settings_set(const char *section, const char *key, int value);

/* Parse `text` as the setting's type and set it: "true", "3", "4:3". A string
 * setting takes the text as it is (leading and trailing blanks removed) and is
 * rejected, with -1, if it does not fit. */
int recomp_settings_set_text(const char *section, const char *key, const char *text);

/* Put a setting back to its default. Same return values as recomp_settings_set. */
int recomp_settings_reset_to_default(const char *section, const char *key);

/* The default as the file writes it ("true", "15", "south", "K, Space"). Returns `buf`. */
const char *recomp_settings_default_text(const RecompSetting *setting, char *buf, size_t size);

/* The value as it is written to the file. Returns `buf`. A string setting is
 * formatted from the setting's own stored text; `value` is not used. */
const char *recomp_settings_format(const RecompSetting *setting, int value,
                                   char *buf, size_t size);

void recomp_settings_on_change(RecompSettingChanged callback, void *user);
void recomp_settings_set_log(void (*log)(const char *message));

/* Forget the table, the unknown keys and the callbacks (for tests). */
void recomp_settings_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_SETTINGS_H */
