#if defined(__ANDROID__)
/*
 * Language packs (docs/language-packs.md).
 *
 * A pack is a mod: a folder in the mods folder (<data>/mods, or
 * RECOMP_TEXTURE_ROOT) with language.ini and small binary deltas, one per
 * translated file or archive entry. It carries only what the translator
 * changed. The launcher's Mods page turns one on ([language] pack); with none
 * the game plays in its own English. At start-up the game rebuilds the files the pack lists
 * from the player's own dump, once, into <data>/cache/language/<pack>, and the
 * kernel's path layer then opens those copies whenever the title asks for the
 * originals (g_xbox_game_file_redirect). The dump itself is never written.
 *
 * Archives are EA BIG files, nested (screens.viv holds a BIG per screen), with
 * entries usually RefPack-compressed. A changed entry is decompressed, patched,
 * stored again as literal RefPack when it was compressed, and appended to its
 * archive; the directory then points at it. Every other entry keeps its bytes
 * and its offset. ZARF's console patch lays archives out the same way.
 *
 * default.xbe is not served from a copy: its code is lifted to C ahead of time.
 * A pack may change bytes in .rdata and .data (an embedded texture, a string),
 * which pc_language_patch_xbe applies to the loaded image before it is mapped.
 *
 * scripts/langpack.py is the reference implementation of the same format; the
 * unit tests check that both produce identical files.
 */
#define _CRT_SECURE_NO_WARNINGS
#include "host.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#ifndef _stricmp
#define _stricmp strcasecmp
#endif
#endif

#include "recomp_settings.h"
#include "pc_language.h"

/* The toolkit's game-file redirect (kernel.h, not on this target's include path). */
#if defined(_WIN32)
typedef WCHAR xbox_host_char;
#else
typedef char xbox_host_char;
#endif
extern BOOL (*g_xbox_game_file_redirect)(const char *relative, xbox_host_char *host_path, DWORD size);

#define LANG_MAX_FILES   256
#define LANG_PATH        1024
#define LANG_MAX_ENTRY   (64u << 20)    /* one decompressed entry */
#define DELTA_HEADER     80

#if defined(__ANDROID__)
enum { STORY_SUBTITLES_DEFAULT = 1 };
#else
enum { STORY_SUBTITLES_DEFAULT = 0 };
#endif

static const char *const k_subtitles[] = { "auto", "all cutscenes", "as the game does", NULL };

size_t pc_language_settings(RecompSetting *out, size_t max)
{
    size_t n = 0;
    if (max < 2) return 0;
    memset(out, 0, sizeof(*out) * 2);
#define ADD(...) do { RecompSetting s_ = { __VA_ARGS__ }; out[n++] = s_; } while (0)
    ADD("language", "pack", RECOMP_SETTING_STRING, 0, 0, 0, NULL, "RECOMP_LANGUAGE_PACK", RECOMP_SETTING_RESTART,
        "Language mod: a folder in the mods folder with language.ini (the Mods page sets it); empty plays the "
        "game in its own English.");
    ADD("language", "story_subtitles", RECOMP_SETTING_ENUM, STORY_SUBTITLES_DEFAULT, 0, 0, k_subtitles, "RECOMP_STORY_SUBTITLES",
        RECOMP_SETTING_RESTART,
        "Subtitles in Story cutscenes: auto follows the language pack (a translation shows them in every "
        "cutscene), or choose.");
#undef ADD
    return n;
}

/* ---- SHA-256 ------------------------------------------------------------------------------------ */

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t used; } Sha;

static const uint32_t k_sha[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha *s, const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + k_sha[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->len = 0;
    s->used = 0;
}

static void sha_update(Sha *s, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    s->len += n;
    while (n) {
        size_t take = 64 - s->used < n ? 64 - s->used : n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}

static void sha_final(Sha *s, uint8_t out[32])
{
    uint64_t bits = s->len * 8;
    uint8_t pad = 0x80, zero = 0, len[8];
    sha_update(s, &pad, 1);
    while (s->used != 56) sha_update(s, &zero, 1);
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) {
        out[4 * i] = (uint8_t)(s->h[i] >> 24); out[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(s->h[i] >> 8); out[4 * i + 3] = (uint8_t)s->h[i];
    }
}

static void sha256(const void *data, size_t n, uint8_t out[32])
{
    Sha s;
    sha_init(&s);
    sha_update(&s, data, n);
    sha_final(&s, out);
}

/* ---- byte buffers and files --------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t n, cap; } Buf;

static int buf_reserve(Buf *b, size_t n)
{
    if (n <= b->cap) return 1;
    size_t cap = b->cap ? b->cap : 4096;
    while (cap < n) cap *= 2;
    uint8_t *p = (uint8_t *)realloc(b->p, cap);
    if (!p) return 0;
    b->p = p;
    b->cap = cap;
    return 1;
}

static int buf_add(Buf *b, const void *data, size_t n)
{
    if (!buf_reserve(b, b->n + n)) return 0;
    if (n) memcpy(b->p + b->n, data, n);
    b->n += n;
    return 1;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void put_be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void put_le32(uint8_t *p, uint32_t v) { p[3] = (uint8_t)(v >> 24); p[2] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[0] = (uint8_t)v; }

static int read_file(const char *path, Buf *out, size_t limit)
{
    FILE *f = fopen(path, "rb");
    long n;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || (size_t)n > limit || fseek(f, 0, SEEK_SET) ||
        !buf_reserve(out, (size_t)n + 1) || fread(out->p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        return 0;
    }
    out->n = (size_t)n;
    fclose(f);
    return 1;
}

static void make_dirs(const char *path)
{
    char tmp[LANG_PATH];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        char c = *p;
        *p = 0;
#if defined(_WIN32)
        _mkdir(tmp);
#else
        mkdir(tmp, 0755);
#endif
        *p = c;
    }
}

static int replace_file(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(from, to) == 0;
#endif
}

/* Case-insensitive lookup below root, as the Xbox file system is. */
static int find_in(const char *root, const char *relative, char *out, size_t size)
{
    char part[256];
    const char *p = relative;
    snprintf(out, size, "%s", root);
    while (*p) {
        size_t k = strcspn(p, "/\\");
        struct stat st;
        if (k == 0 || k >= sizeof part) return 0;
        memcpy(part, p, k);
        part[k] = 0;
        p += k + (p[k] != 0);
        size_t used = strlen(out);
        snprintf(out + used, size - used, HOST_SEP "%s", part);
        if (stat(out, &st) == 0) continue;
        out[used] = 0;
        int found = 0;
#if defined(_WIN32)
        /* Windows names ignore case already; a miss is a miss. */
#else
        DIR *dir = opendir(out);
        struct dirent *e;
        while (dir && (e = readdir(dir)))
            if (!strcasecmp(e->d_name, part)) {
                snprintf(out + used, size - used, "/%s", e->d_name);
                found = 1;
                break;
            }
        if (dir) closedir(dir);
#endif
        if (!found) return 0;
    }
    return 1;
}

/* ---- RefPack, BIG archives, deltas -------------------------------------------------------------- */

static char g_error[512];

static int fail(const char *fmt, const char *what)
{
    snprintf(g_error, sizeof g_error, fmt, what ? what : "");
    return 0;
}

static int is_refpack(const uint8_t *p, size_t n)
{
    return n >= 5 && ((p[0] << 8 | p[1]) & 0x3EFF) == 0x10FB;
}

static int refpack_decompress(const uint8_t *src, size_t n, Buf *out)
{
    unsigned sig = (unsigned)(src[0] << 8 | src[1]);
    size_t pos = 2, step = (sig & 0x8000) ? 4 : 3, size = 0;
    if (sig & 0x0100) pos += step;
    if (pos + step > n) return fail("truncated RefPack header%s", NULL);
    for (size_t i = 0; i < step; i++) size = size << 8 | src[pos + i];
    pos += step;
    if (size > LANG_MAX_ENTRY || !buf_reserve(out, size + 4)) return fail("RefPack entry too large%s", NULL);
    out->n = 0;
    while (pos < n) {
        unsigned b0 = src[pos++], b1, b2, b3;
        size_t run, length = 0, dist = 0;
        if (b0 < 0x80) {
            if (pos + 1 > n) break;
            b1 = src[pos++];
            run = b0 & 3; length = ((b0 & 0x1C) >> 2) + 3; dist = ((b0 & 0x60) << 3) + b1 + 1;
        } else if (b0 < 0xC0) {
            if (pos + 2 > n) break;
            b1 = src[pos]; b2 = src[pos + 1]; pos += 2;
            run = (b1 >> 6) & 3; length = (b0 & 0x3F) + 4; dist = ((b1 & 0x3F) << 8) + b2 + 1;
        } else if (b0 < 0xE0) {
            if (pos + 3 > n) break;
            b1 = src[pos]; b2 = src[pos + 1]; b3 = src[pos + 2]; pos += 3;
            run = b0 & 3; length = ((b0 & 0x0C) << 6) + b3 + 5; dist = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1;
        } else if (b0 < 0xFC) {
            run = ((b0 & 0x1F) << 2) + 4;
        } else {
            run = b0 & 3;
        }
        if (pos + run > n || out->n + run + length > size + 3 || dist > out->n + run)
            return fail("corrupt RefPack stream%s", NULL);
        memcpy(out->p + out->n, src + pos, run);
        out->n += run;
        pos += run;
        for (size_t i = 0; i < length; i++, out->n++)
            out->p[out->n] = out->p[out->n - dist];
        if (b0 >= 0xFC) break;
    }
    /* Up to three bytes over is ZARF's padded last block; the header's size is the data. */
    if (out->n < size) return fail("short RefPack stream%s", NULL);
    out->n = size;
    return 1;
}

static int refpack_store(const uint8_t *data, size_t n, Buf *out)
{
    uint8_t head[6];
    size_t pos = 0, h = 0;
    out->n = 0;
    if (n < (1u << 24)) { head[0] = 0x10; head[1] = 0xFB; head[2] = (uint8_t)(n >> 16); head[3] = (uint8_t)(n >> 8); head[4] = (uint8_t)n; h = 5; }
    else { head[0] = 0x90; head[1] = 0xFB; put_be32(head + 2, (uint32_t)n); h = 6; }
    if (!buf_reserve(out, n + n / 112 + 16) || !buf_add(out, head, h)) return 0;
    while (n - pos >= 4) {
        size_t run = n - pos >= 112 ? 112 : (n - pos) & ~(size_t)3;
        uint8_t c = (uint8_t)(0xE0 + ((run - 4) >> 2));
        buf_add(out, &c, 1);
        buf_add(out, data + pos, run);
        pos += run;
    }
    uint8_t end = (uint8_t)(0xFC + (n - pos));
    buf_add(out, &end, 1);
    buf_add(out, data + pos, n - pos);
    return 1;
}

typedef struct { char name[256]; uint32_t off, size; size_t dirpos; } BigEntry;

/* Parse the directory of a BIG archive held in p[0..n) (n may be just the header). */
static int big_entries(const uint8_t *p, size_t n, size_t file_size, BigEntry **out, uint32_t *count)
{
    if (n < 16 || (memcmp(p, "BIGF", 4) && memcmp(p, "BIG4", 4))) return fail("not a BIG archive%s", NULL);
    uint32_t c = be32(p + 8);
    size_t pos = 16;
    BigEntry *e = (BigEntry *)calloc(c ? c : 1, sizeof *e);
    if (!e || c > 1000000) { free(e); return fail("bad archive directory%s", NULL); }
    for (uint32_t i = 0; i < c; i++) {
        const uint8_t *z;
        if (pos + 9 > n || !(z = (const uint8_t *)memchr(p + pos + 8, 0, n - pos - 8)) ||
            (size_t)(z - (p + pos + 8)) >= sizeof e[i].name) {
            free(e);
            return fail("bad archive directory%s", NULL);
        }
        e[i].off = be32(p + pos);
        e[i].size = be32(p + pos + 4);
        e[i].dirpos = pos;
        memcpy(e[i].name, p + pos + 8, (size_t)(z - (p + pos + 8)));
        if ((uint64_t)e[i].off + e[i].size > file_size) {
            fail("archive entry %s runs past the end", e[i].name);
            free(e);
            return 0;
        }
        pos = (size_t)(z - p) + 1;
    }
    *out = e;
    *count = c;
    return 1;
}

static uint32_t big_alignment(const BigEntry *e, uint32_t count)
{
    uint32_t align = 4096;
    for (uint32_t i = 0; i < count && align > 1; i++)
        while (align > 1 && e[i].off % align) align /= 2;
    return align;
}

static int delta_apply(const uint8_t *d, size_t dn, const uint8_t *src, size_t sn, Buf *out, const char *what)
{
    uint8_t hash[32];
    if (dn < DELTA_HEADER + 1 || memcmp(d, "DJDELTA1", 8)) return fail("%s: not a language pack delta", what);
    uint32_t src_size = le32(d + 8), dst_size = le32(d + 12);
    if (sn != src_size) return fail("%s: your dump's file differs from the one the pack was made for", what);
    sha256(src, sn, hash);
    if (memcmp(hash, d + 16, 32)) return fail("%s: your dump's file differs from the one the pack was made for", what);
    if (dst_size > LANG_MAX_ENTRY && dst_size > sn) return fail("%s: delta result too large", what);
    out->n = 0;
    if (!buf_reserve(out, dst_size + 1)) return fail("%s: out of memory", what);
    size_t pos = DELTA_HEADER;
    for (;;) {
        if (pos >= dn) return fail("%s: delta has no end marker", what);
        uint8_t op = d[pos++];
        if (op == 0) break;
        if (op == 1 && pos + 8 <= dn) {
            uint32_t off = le32(d + pos), len = le32(d + pos + 4);
            pos += 8;
            if ((uint64_t)off + len > sn || out->n + len > dst_size) return fail("%s: corrupt delta", what);
            memcpy(out->p + out->n, src + off, len);
            out->n += len;
        } else if (op == 2 && pos + 4 <= dn) {
            uint32_t len = le32(d + pos);
            pos += 4;
            if (pos + len > dn || out->n + len > dst_size) return fail("%s: corrupt delta", what);
            memcpy(out->p + out->n, d + pos, len);
            out->n += len;
            pos += len;
        } else {
            return fail("%s: corrupt delta", what);
        }
    }
    sha256(out->p, out->n, hash);
    if (out->n != dst_size || memcmp(hash, d + 48, 32)) return fail("%s: delta result does not match its hash", what);
    return 1;
}

/* ---- the pack ----------------------------------------------------------------------------------- */

typedef struct {
    char target[512];       /* "screens/screens.viv|screens/intMain/mainMenu.big|mainMenu.xsh" */
    Buf delta;
} PackFile;

typedef struct {
    char folder[64], name[96], language[16];
    int all_subtitles;
    PackFile files[LANG_MAX_FILES];
    int count;
    Buf ini;
} Pack;

typedef struct { char relative[256]; char host[LANG_PATH]; } Served;

static Pack g_pack;
static Served g_served[16];
static int g_served_count;
static int g_active;
static int g_pack_subtitles;

static void pack_free(Pack *p)
{
    for (int i = 0; i < p->count; i++) free(p->files[i].delta.p);
    free(p->ini.p);
    memset(p, 0, sizeof *p);
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = 0;
    return s;
}

/* The next sep-separated token of *cursor (modified in place), or NULL at the end. */
static char *next_token(char **cursor, char sep)
{
    char *start = *cursor, *end;
    if (!start || !*start) return NULL;
    end = strchr(start, sep);
    if (end) { *end = 0; *cursor = end + 1; }
    else *cursor = start + strlen(start);
    return start;
}

static int safe_name(const char *s, int allow_slash)
{
    if (!*s || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (const char *p = s; *p; p++)
        if (!(isalnum((unsigned char)*p) || *p == '_' || *p == '-' || *p == '.' || (allow_slash && *p == '/')))
            return 0;
    return !strstr(s, "..");
}

static int valid_target(const char *t)
{
    char copy[512], *cursor = copy, *part;
    snprintf(copy, sizeof copy, "%s", t);
    while ((part = next_token(&cursor, '|')) != NULL) {
        part = trim(part);
        if (!safe_name(part, 1) || part[0] == '/' || part[strlen(part) - 1] == '/') return 0;
    }
    return t[0] && t[strlen(t) - 1] != '|';
}

/* Parse language.ini and read every delta. Returns 0 with g_error set. */
static int pack_load(const char *dir, const char *folder, Pack *p, int read_deltas)
{
    char path[LANG_PATH], *line, *cursor, section[32] = "";
    int schema = 0, xbe_targets = 0;
    memset(p, 0, sizeof *p);
    snprintf(p->folder, sizeof p->folder, "%s", folder);
    snprintf(path, sizeof path, "%s" HOST_SEP "language.ini", dir);
    if (!read_file(path, &p->ini, 1u << 20)) return fail("%s: no language.ini", folder);
    p->ini.p[p->ini.n] = 0;
    char *text = (char *)malloc(p->ini.n + 1);
    if (!text) return fail("out of memory%s", NULL);
    memcpy(text, p->ini.p, p->ini.n + 1);
    char *start = text;
    if (p->ini.n >= 3 && !memcmp(start, "\xEF\xBB\xBF", 3)) start += 3;
    cursor = start;
    while ((line = next_token(&cursor, '\n')) != NULL) {
        line = trim(line);
        if (!*line || *line == ';' || *line == '#') continue;
        if (*line == '[') {
            snprintf(section, sizeof section, "%s", line);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = trim(line), *value = trim(eq + 1);
        if (!strcmp(section, "[language]")) {
            if (!strcmp(key, "schema")) schema = atoi(value);
            else if (!strcmp(key, "name")) snprintf(p->name, sizeof p->name, "%s", value);
            else if (!strcmp(key, "language")) snprintf(p->language, sizeof p->language, "%s", value);
            else if (!strcmp(key, "story_subtitles")) p->all_subtitles = !strcmp(value, "all");
        } else if (!strcmp(section, "[files]")) {
            size_t vlen = strlen(value);
            if (p->count >= LANG_MAX_FILES) { free(text); return fail("%s: too many files", folder); }
            if (strlen(key) >= sizeof(p->files[0].target) || !valid_target(key) || !safe_name(value, 1) || vlen < 5 || strcmp(value + vlen - 4, ".djd")) {
                fail("bad [files] line: %s", key);
                free(text);
                return 0;
            }
            if (!strncasecmp(key, "default.xbe", 11) && (key[11] == 0 || key[11] == '|')) {
                if (key[11] || ++xbe_targets > 1) {
                    fail("unsupported or duplicate XBE change: %s", key);
                    free(text);
                    return 0;
                }
            }
            PackFile *f = &p->files[p->count++];
            snprintf(f->target, sizeof f->target, "%s", key);
            if (read_deltas) {
                snprintf(path, sizeof path, "%s" HOST_SEP "%s", dir, value);
                if (!read_file(path, &f->delta, 256u << 20)) {
                    fail("missing delta %s", value);
                    free(text);
                    return 0;
                }
            }
        }
    }
    free(text);
    if (schema != 1) return fail("%s: [language] schema=1 is required", folder);
    if (!p->name[0]) snprintf(p->name, sizeof p->name, "%s", folder);
    if (!p->count) return fail("%s: [files] lists nothing", folder);
    return 1;
}

/* The first path component of a target, and the rest. */
static void split_target(const char *target, char *file, size_t fsize, const char **rest)
{
    size_t k = strcspn(target, "|");
    snprintf(file, fsize, "%.*s", (int)k, target);
    *rest = target[k] ? target + k + 1 : "";
}

typedef struct { const char *path; const Buf *delta; } Edit;

/* Rebuild an in-memory blob (an entry, already decompressed) for edits relative to it. */
static int rebuild_blob(const uint8_t *blob, size_t n, Edit *edits, int count, Buf *out)
{
    if (count == 1 && !edits[0].path[0])
        return delta_apply(edits[0].delta->p, edits[0].delta->n, blob, n, out, "an entry");
    for (int i = 0; i < count; i++)
        if (!edits[i].path[0]) return fail("a target and an entry inside it are both listed%s", NULL);
    BigEntry *e;
    uint32_t c, align;
    if (!big_entries(blob, n, n, &e, &c)) return 0;
    align = big_alignment(e, c);
    out->n = 0;
    if (!buf_add(out, blob, n)) { free(e); return 0; }
    int done = 0;
    char *used = (char *)calloc(count, 1);
    for (uint32_t i = 0; i < c && done < count; i++) {
        Edit sub[LANG_MAX_FILES];
        int k = 0;
        for (int j = 0; j < count; j++) {
            char name[256];
            const char *rest;
            split_target(edits[j].path, name, sizeof name, &rest);
            if (!used[j] && !_stricmp(name, e[i].name)) {
                sub[k].path = rest;
                sub[k++].delta = edits[j].delta;
                used[j] = 1;
            }
        }
        if (!k) continue;
        done += k;
        Buf raw = {0}, inner = {0}, packed = {0};
        const uint8_t *entry = blob + e[i].off;
        int compressed = is_refpack(entry, e[i].size);
        int ok = compressed ? refpack_decompress(entry, e[i].size, &raw) : buf_add(&raw, entry, e[i].size);
        ok = ok && rebuild_blob(raw.p, raw.n, sub, k, &inner);
        ok = ok && (compressed ? refpack_store(inner.p, inner.n, &packed) : buf_add(&packed, inner.p, inner.n));
        if (ok) {
            static const uint8_t zero[4096];
            buf_add(out, zero, (align - out->n % align) % align);
            put_be32(out->p + e[i].dirpos, (uint32_t)out->n);
            put_be32(out->p + e[i].dirpos + 4, (uint32_t)packed.n);
            ok = buf_add(out, packed.p, packed.n);
        }
        free(raw.p); free(inner.p); free(packed.p);
        if (!ok) { free(e); free(used); return 0; }
    }
    if (done != count) {
        for (int j = 0; j < count; j++)
            if (!used[j]) { fail("no entry %s", edits[j].path); break; }
        free(e); free(used);
        return 0;
    }
    put_le32(out->p + 4, (uint32_t)out->n);
    free(e);
    free(used);
    return 1;
}

/* Rebuild one file of the dump into out_path. Archives are streamed: the original
 * is copied, the changed entries appended and the directory rewritten, so
 * screens.viv (97 MB) never sits in memory whole. */
static int rebuild_file(const char *source, const char *out_path, Edit *edits, int count)
{
    char part[LANG_PATH];
    FILE *in = fopen(source, "rb"), *out = NULL;
    uint8_t head[16];
    Buf dir = {0}, built = {0};
    BigEntry *e = NULL;
    uint32_t c = 0;
    long size;
    int ok = 0;

    if (!in) return fail("cannot open %s", source);
    fseek(in, 0, SEEK_END);
    size = ftell(in);
    fseek(in, 0, SEEK_SET);
    snprintf(part, sizeof part, "%s.part", out_path);
    make_dirs(out_path);

    if (count == 1 && !edits[0].path[0]) {
        /* A whole file (not an archive). */
        Buf src = {0};
        fclose(in);
        in = NULL;
        ok = read_file(source, &src, 512u << 20) &&
             delta_apply(edits[0].delta->p, edits[0].delta->n, src.p, src.n, &built, source);
        free(src.p);
        if (ok && ((out = fopen(part, "wb")) == NULL || fwrite(built.p, 1, built.n, out) != built.n)) ok = 0;
        goto done;
    }
    if (size < 16 || fread(head, 1, 16, in) != 16) { fail("cannot read %s", source); goto done; }
    {
        uint32_t header = be32(head + 12);
        if (header < 16 || header > (64u << 20) || header > (uint32_t)size || !buf_reserve(&dir, header)) {
            fail("%s: not an archive, but entries inside it are listed", source);
            goto done;
        }
        memcpy(dir.p, head, 16);
        if (fread(dir.p + 16, 1, header - 16, in) != header - 16) { fail("cannot read %s", source); goto done; }
        dir.n = header;
    }
    if (!big_entries(dir.p, dir.n, (size_t)size, &e, &c)) goto done;
    out = fopen(part, "wb");
    if (!out) { fail("cannot write %s", part); goto done; }
    {
        /* Copy the original as it is. */
        static uint8_t chunk[1 << 20];
        size_t got;
        fseek(in, 0, SEEK_SET);
        while ((got = fread(chunk, 1, sizeof chunk, in)) > 0)
            if (fwrite(chunk, 1, got, out) != got) { fail("cannot write %s", part); goto done; }
    }
    {
        uint32_t align = big_alignment(e, c);
        uint64_t end = (uint64_t)size;
        int done = 0;
        char *used = (char *)calloc(count, 1);
        for (uint32_t i = 0; i < c && done < count; i++) {
            Edit sub[LANG_MAX_FILES];
            int k = 0;
            for (int j = 0; j < count; j++) {
                char name[256];
                const char *rest;
                split_target(edits[j].path, name, sizeof name, &rest);
                if (!used[j] && !_stricmp(name, e[i].name)) {
                    sub[k].path = rest;
                    sub[k++].delta = edits[j].delta;
                    used[j] = 1;
                }
            }
            if (!k) continue;
            done += k;
            Buf entry = {0}, raw = {0}, inner = {0}, packed = {0};
            int good = buf_reserve(&entry, e[i].size + 1) && fseek(in, (long)e[i].off, SEEK_SET) == 0 &&
                       fread(entry.p, 1, e[i].size, in) == e[i].size;
            int compressed = good && is_refpack(entry.p, e[i].size);
            if (good) {
                entry.n = e[i].size;
                good = compressed ? refpack_decompress(entry.p, entry.n, &raw) : buf_add(&raw, entry.p, entry.n);
            }
            good = good && rebuild_blob(raw.p, raw.n, sub, k, &inner);
            good = good && (compressed ? refpack_store(inner.p, inner.n, &packed) : buf_add(&packed, inner.p, inner.n));
            if (good) {
                static const uint8_t zero[4096];
                uint32_t pad = (uint32_t)((align - end % align) % align);
                good = fwrite(zero, 1, pad, out) == pad && fwrite(packed.p, 1, packed.n, out) == packed.n;
                put_be32(dir.p + e[i].dirpos, (uint32_t)(end + pad));
                put_be32(dir.p + e[i].dirpos + 4, (uint32_t)packed.n);
                end += pad + packed.n;
            }
            free(entry.p); free(raw.p); free(inner.p); free(packed.p);
            if (!good) { free(used); if (!g_error[0]) fail("cannot rebuild %s", source); goto done; }
        }
        if (done != count) {
            for (int j = 0; j < count; j++)
                if (!used[j]) { fail("no entry %s", edits[j].path); break; }
            free(used);
            goto done;
        }
        free(used);
        if (end > 0xFFFFFFFFull) { fail("%s would pass 4 GiB", source); goto done; }
        put_le32(dir.p + 4, (uint32_t)end);
        if (fseek(out, 0, SEEK_SET) || fwrite(dir.p, 1, dir.n, out) != dir.n) { fail("cannot write %s", part); goto done; }
    }
    ok = 1;
done:
    if (in) fclose(in);
    if (out && fclose(out)) ok = 0;
    free(dir.p);
    free(built.p);
    free(e);
    if (ok && !replace_file(part, out_path)) ok = fail("cannot replace %s", out_path);
    if (!ok) remove(part);
    return ok;
}

/* ---- start-up ----------------------------------------------------------------------------------- */

static int same_file_name(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a == '\\' ? '/' : (char)tolower((unsigned char)*a);
        char y = *b == '\\' ? '/' : (char)tolower((unsigned char)*b);
        if (x != y) return 0;
    }
    return *a == *b;
}

static BOOL lang_redirect(const char *relative, xbox_host_char *host_path, DWORD size)
{
    for (int i = 0; i < g_served_count; i++) {
        if (!same_file_name(relative, g_served[i].relative)) continue;
#if defined(_WIN32)
        if (!MultiByteToWideChar(CP_ACP, 0, g_served[i].host, -1, host_path, (int)size)) return FALSE;
#else
        if ((size_t)snprintf(host_path, size, "%s", g_served[i].host) >= size) return FALSE;
#endif
        return TRUE;
    }
    return FALSE;
}

static void hex(const uint8_t *h, char *out)
{
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", h[i]);
}

/* A pack's own images: a manifest.ini beside language.ini, in the texture-pack format (HD versions of
 * the images it translates). The texture loader reads RECOMP_TEXTURE_PACKS once, at its first texture;
 * the pack goes first there, so it ranks beneath every texture pack (later names win). */
static void language_texture_layer(const char *dir, const char *folder)
{
    char manifest[LANG_PATH], list[4096];
    const char *old = getenv("RECOMP_TEXTURE_PACKS");
    FILE *f;
    snprintf(manifest, sizeof manifest, "%s" HOST_SEP "manifest.ini", dir);
    if (!(f = fopen(manifest, "rb"))) return;
    fclose(f);
    if (old && *old) {
        size_t n = strlen(folder);
        for (const char *p = old; p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL)
            if (!strncmp(p, folder, n) && (p[n] == ';' || !p[n])) return;     /* listed by hand already */
    }
    if (snprintf(list, sizeof list, "%s%s%s", folder, old && *old ? ";" : "", old ? old : "") >= (int)sizeof list) {
        fprintf(stderr, "[LANG] %s: its images are not used (texture pack list too long)\n", folder);
        return;
    }
#if defined(_WIN32)
    _putenv_s("RECOMP_TEXTURE_PACKS", list);
#else
    setenv("RECOMP_TEXTURE_PACKS", list, 1);
#endif
    fprintf(stderr, "[LANG] %s: its own images load beneath the texture packs\n", folder);
}

void pc_language_start(const char *data_root, const char *game_dir)
{
    char folder[RECOMP_SETTING_TEXT_MAX], dir[LANG_PATH], cache[LANG_PATH], stamp_path[LANG_PATH];
    char files[16][256];
    int nfiles = 0;
    uint8_t digest[32];
    char stamp[65], old[65] = "";
    Sha sha;

    folder[0] = 0;
    recomp_settings_get_text("language", "pack", folder, sizeof folder, "");
    {
        char *trimmed = trim(folder);
        if (trimmed != folder) memmove(folder, trimmed, strlen(trimmed) + 1);
    }
    if (!folder[0]) {
        fprintf(stderr, "[LANG] the game's own language (no language pack)\n");
        return;
    }
    if (!data_root || !*data_root) {
        fprintf(stderr, "[LANG] language pack '%s' needs a data folder; playing in the game's own language\n", folder);
        return;
    }
    if (!safe_name(folder, 0) || strlen(folder) >= sizeof g_pack.folder) {
        fprintf(stderr, "[LANG] '%s' is not a pack folder name; playing in the game's own language\n", folder);
        return;
    }
    {
        /* The mods folder, as the launcher's Mods page and the texture loader find it. */
        const char *mods = getenv("RECOMP_TEXTURE_ROOT");
        if (mods && *mods) snprintf(dir, sizeof dir, "%s" HOST_SEP "%s", mods, folder);
        else snprintf(dir, sizeof dir, "%s" HOST_SEP "mods" HOST_SEP "%s", data_root, folder);
    }
    g_error[0] = 0;
    if (!pack_load(dir, folder, &g_pack, 1)) {
        fprintf(stderr, "[LANG] language pack '%s' not used: %s\n", folder, g_error);
        pack_free(&g_pack);
        return;
    }

    /* Which files of the dump it rebuilds (default.xbe is patched in memory instead). */
    for (int i = 0; i < g_pack.count; i++) {
        char file[256];
        const char *rest;
        int seen = 0;
        split_target(g_pack.files[i].target, file, sizeof file, &rest);
        for (int j = 0; j < nfiles; j++) seen |= same_file_name(files[j], file);
        if (!seen && _stricmp(file, "default.xbe")) {
            if (nfiles >= 16) {
                fprintf(stderr, "[LANG] language pack '%s' not used: more than 16 game files\n", folder);
                pack_free(&g_pack);
                return;
            }
            snprintf(files[nfiles++], sizeof files[0], "%s", file);
        }
    }

    /* The cache is valid while the pack and the dump files it reads are unchanged. */
    sha_init(&sha);
    sha_update(&sha, "DJLANG1", 8);
    sha_update(&sha, g_pack.ini.p, g_pack.ini.n);
    for (int i = 0; i < g_pack.count; i++) sha_update(&sha, g_pack.files[i].delta.p, g_pack.files[i].delta.n);
    for (int j = 0; j < nfiles; j++) {
        char source[LANG_PATH];
        struct stat st;
        long long meta[2] = { -1, -1 };
        if (find_in(game_dir, files[j], source, sizeof source) && stat(source, &st) == 0) {
            meta[0] = (long long)st.st_size;
            meta[1] = (long long)st.st_mtime;
        }
        sha_update(&sha, files[j], strlen(files[j]) + 1);
        sha_update(&sha, meta, sizeof meta);
    }
    sha_final(&sha, digest);
    hex(digest, stamp);
    snprintf(cache, sizeof cache, "%s" HOST_SEP "cache" HOST_SEP "language" HOST_SEP "%s", data_root, folder);
    snprintf(stamp_path, sizeof stamp_path, "%s" HOST_SEP "stamp.txt", cache);
    {
        FILE *f = fopen(stamp_path, "r");
        if (f) {
            if (!fgets(old, sizeof old, f)) old[0] = 0;
            fclose(f);
        }
    }

    int fresh = !strcmp(old, stamp);
    for (int j = 0; j < nfiles && fresh; j++) {
        char out[LANG_PATH];
        struct stat st;
        snprintf(out, sizeof out, "%s" HOST_SEP "%s", cache, files[j]);
        fresh = stat(out, &st) == 0;
    }
    if (!fresh) {
        fprintf(stderr, "[LANG] building '%s' (%s) from your dump: %d change(s) in %d file(s)\n",
                g_pack.name, folder, g_pack.count, nfiles);
        remove(stamp_path);
        for (int j = 0; j < nfiles; j++) {
            char source[LANG_PATH], out[LANG_PATH];
            Edit edits[LANG_MAX_FILES];
            int k = 0;
            for (int i = 0; i < g_pack.count; i++) {
                char file[256];
                const char *rest;
                split_target(g_pack.files[i].target, file, sizeof file, &rest);
                if (same_file_name(file, files[j])) { edits[k].path = rest; edits[k++].delta = &g_pack.files[i].delta; }
            }
            snprintf(out, sizeof out, "%s" HOST_SEP "%s", cache, files[j]);
            g_error[0] = 0;
            if (!find_in(game_dir, files[j], source, sizeof source)) fail("%s is not in the game folder", files[j]);
            if (!g_error[0] && !rebuild_file(source, out, edits, k) && !g_error[0])
                fail("could not rebuild %s", source);
            if (g_error[0]) {
                fprintf(stderr, "[LANG] language pack '%s' not used: %s\n", folder, g_error);
                pack_free(&g_pack);
                return;
            }
        }
        make_dirs(stamp_path);
        FILE *f = fopen(stamp_path, "w");
        if (f) { fprintf(f, "%s\n", stamp); fclose(f); }
    }

    for (int j = 0; j < nfiles; j++) {
        Served *s = &g_served[g_served_count++];
        snprintf(s->relative, sizeof s->relative, "%s", files[j]);
        snprintf(s->host, sizeof s->host, "%s" HOST_SEP "%s", cache, files[j]);
#if defined(_WIN32)
        for (char *p = s->host; *p; p++) if (*p == '/') *p = '\\';
#endif
    }
    g_pack_subtitles = g_pack.all_subtitles;
    g_active = 1;
    g_xbox_game_file_redirect = lang_redirect;
    fprintf(stderr, "[LANG] %s (%s, %s): %d file(s) served from %s%s\n", g_pack.name, g_pack.language, folder,
            g_served_count, cache, fresh ? "" : " (built now)");
    language_texture_layer(dir, folder);
    /* Keep only what pc_language_patch_xbe still needs. */
    for (int i = 0; i < g_pack.count; i++) {
        char file[256];
        const char *rest;
        split_target(g_pack.files[i].target, file, sizeof file, &rest);
        if (_stricmp(file, "default.xbe")) { free(g_pack.files[i].delta.p); memset(&g_pack.files[i].delta, 0, sizeof(Buf)); }
    }
}

void pc_language_patch_xbe(void *xbe, size_t size)
{
    static const char *const allowed[] = { ".rdata", ".data" };
    uint8_t *image = (uint8_t *)xbe;
    if (!g_active) return;
    for (int i = 0; i < g_pack.count; i++) {
        PackFile *f = &g_pack.files[i];
        Buf out = {0};
        if (_stricmp(f->target, "default.xbe") || !f->delta.p) continue;
        g_error[0] = 0;
        if (!delta_apply(f->delta.p, f->delta.n, image, size, &out, "default.xbe") || out.n != size) {
            fprintf(stderr, "[LANG] default.xbe change not applied: %s\n", g_error[0] ? g_error : "size differs");
            free(out.p);
            g_xbox_game_file_redirect = NULL;
            g_active = 0;
            g_pack_subtitles = 0;
            return;
        }
        /* Only the data sections: the code was lifted ahead of time, so a change there would
         * not run, and the loaded image must keep matching the lifted code. */
        if (size < 0x124) {
            fprintf(stderr, "[LANG] default.xbe change not applied: truncated header\n");
            free(out.p); g_xbox_game_file_redirect = NULL; g_active = 0; g_pack_subtitles = 0;
            return;
        }
        uint32_t base = le32(image + 0x104), nsec = le32(image + 0x11C), hdr = le32(image + 0x120) - base;
        if ((uint64_t)hdr + (uint64_t)nsec * 56 > size || nsec > 1024) {
            fprintf(stderr, "[LANG] default.xbe change not applied: invalid section table\n");
            free(out.p); g_xbox_game_file_redirect = NULL; g_active = 0; g_pack_subtitles = 0;
            return;
        }
        size_t changed = 0;
        for (size_t at = 0; at < size; at++) {
            if (image[at] == out.p[at]) continue;
            int ok = 0;
            for (uint32_t s = 0; s < nsec && !ok && hdr + (s + 1) * 56 <= size; s++) {
                const uint8_t *h = image + hdr + s * 56;
                uint32_t raw = le32(h + 12), raw_size = le32(h + 16), name = le32(h + 20) - base;
                for (size_t a = 0; a < 2 && name < size; a++)
                    if ((size_t)name + strlen(allowed[a]) + 1 <= size &&
                        !strncmp((const char *)image + name, allowed[a], strlen(allowed[a]) + 1) &&
                        at >= raw && at < (size_t)raw + raw_size)
                        ok = 1;
            }
            if (!ok) {
                fprintf(stderr, "[LANG] default.xbe change not applied: byte 0x%zX is outside .rdata/.data\n", at);
                free(out.p);
                g_xbox_game_file_redirect = NULL;
                g_active = 0;
                g_pack_subtitles = 0;
                return;
            }
            changed++;
        }
        memcpy(image, out.p, size);
        free(out.p);
        fprintf(stderr, "[LANG] default.xbe: %zu data byte(s) changed in memory\n", changed);
    }
}

int pc_language_all_subtitles(void)
{
    int choice = recomp_settings_get("language", "story_subtitles", STORY_SUBTITLES_DEFAULT);
    if (choice == 1) return 1;
    if (choice == 2) return 0;
    return g_active && g_pack_subtitles;
}

int pc_language_french(void)
{
    static int logged;
    if (recomp_settings_get("language", "game", 0) != 1) return 0;
    if (g_active) {
        if (!logged++)
            fprintf(stderr, "[LANG] game language french not used: the language mod '%s' is on\n", g_pack.name);
        return 0;
    }
    return 1;
}

#endif /* __ANDROID__ */
