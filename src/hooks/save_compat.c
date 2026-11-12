/* Existing profiles must remain readable after the correct upstream SHRD lift.
 * The old compiled hash formatter ORed its two words for the final nibble.
 * Prefer the canonical folder; use the precise legacy spelling only if its
 * SaveMeta.xbx identifies this exact name. This never renames or writes files.
 */
#include "save_compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#endif

static char s_save_dir[1024];

void defjam_save_compat_init(const char *user_data_dir)
{
    s_save_dir[0] = 0;
    if (user_data_dir && strlen(user_data_dir) < sizeof(s_save_dir) - 80)
        strcpy(s_save_dir, user_data_dir);
}

static int path_exists(const char *path)
{
#ifdef _WIN32
    wchar_t wide[1104];
    struct _stat info;
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1104)) return 1;
    if (_wstat(wide, &info) == 0) return 1;
#else
    struct stat info;
    if (stat(path, &info) == 0) return 1;
#endif
    /* A failed inspection is not proof that the canonical path is absent. */
    return errno != ENOENT && errno != ENOTDIR;
}

static int metadata_matches(const char *path, const uint16_t *name, size_t length)
{
    unsigned char bytes[2 + 2 * (5 + 128 + 2)];
    static const unsigned char prefix[] = {0xff, 0xfe, 'N', 0, 'a', 0, 'm', 0, 'e', 0, '=', 0};
    FILE *file;
    size_t count, i, start = sizeof(prefix);
    unsigned end;
    if (length > 128)
        return 0;
#ifdef _WIN32
    {
        wchar_t wide[1104];
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 1104)) return 0;
        file = _wfopen(wide, L"rb");
    }
#else
    file = fopen(path, "rb");
#endif
    if (!file) return 0;
    count = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    if ((count & 1) || count < start + 2 * (length + 1)
        || memcmp(bytes, prefix, start))
        return 0;
    for (i = 0; i < length; ++i)
        if ((uint16_t)(bytes[start + 2*i] | (bytes[start + 2*i + 1] << 8)) != name[i])
            return 0;
    end = bytes[start + 2*length] | (bytes[start + 2*length + 1] << 8);
    return end == '\r' || end == '\n' || end == 0;
}

int defjam_save_directory_name(const uint16_t *name, char out[13])
{
    static const char hex[] = "0123456789ABCDEF";
    uint64_t hash = 0;
    size_t length = 0;
    unsigned i, last;
    char legacy[13], path[1104];
    while (name[length]) {
        hash = (hash * UINT64_C(65536) + name[length]) % UINT64_C(0xFFFFFFFFFFC5);
        ++length;
    }
    for (i = 0; i < 12; ++i)
        out[11-i] = hex[(hash >> (4*i)) & 15];
    out[12] = 0;
    /* Only the first, count-zero nibble in the old formatter was corrupted. */
    last = (unsigned)((hash | (hash >> 32)) & 15);
    if (!s_save_dir[0] || out[11] == hex[last])
        return 0;
    snprintf(path, sizeof(path), "%s/45410049/%s", s_save_dir, out);
    if (path_exists(path))
        return 0;
    memcpy(legacy, out, sizeof(legacy));
    legacy[11] = hex[last];
    snprintf(path, sizeof(path), "%s/45410049/%s/SaveMeta.xbx", s_save_dir, legacy);
    if (!metadata_matches(path, name, length))
        return 0;
    memcpy(out, legacy, sizeof(legacy));
    return 1;
}
