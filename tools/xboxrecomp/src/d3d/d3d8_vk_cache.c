/*
 * d3d8_vk_cache.c -- the Vulkan device's shader and pipeline cache on disk
 * (d3d8_vk_cache.h says what is kept and how a bad file is treated).
 */
#include "d3d8_vk_cache.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#define vkc_mkdir(p)  _mkdir(p)
#define vkc_getpid()  _getpid()
#else
#include <unistd.h>
#define vkc_mkdir(p)  mkdir((p), 0755)
#define vkc_getpid()  getpid()
#endif

#define SPV_MAGIC   "XRSPIRV1"
#define PIPE_MAGIC  "XRPIPEC1"
#define PIPE_FORMAT 1u
#define SPV_MAX     (16u << 20)
#define PIPE_MAX    (256u << 20)
#define SPIRV_WORD0 0x07230203u

typedef struct {
    char     magic[8];
    uint64_t seed;
    uint64_t key;           /* the file's name */
    uint64_t check;         /* the text again, hashed from another start: a key collision is a miss */
    uint64_t text_len;
    uint32_t stage, bytes;
    uint64_t payload;       /* hash of the SPIR-V */
} SpvHeader;

typedef struct {
    char      magic[8];
    uint32_t  format, bytes;
    uint64_t  payload;
    VkcDevice dev;
    uint32_t  pad;
} PipeHeader;

static struct {
    int      on;
    uint64_t seed;
    char     dir[1024];
} s;

uint64_t vkc_hash64(const void *p, size_t n, uint64_t h)
{
    const uint8_t *b = (const uint8_t *)p;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* mkdir -p */
static int make_dirs(const char *path)
{
    char buf[1024];
    size_t i, n = strlen(path);
    struct stat st;

    if (n == 0 || n >= sizeof buf)
        return 0;
    memcpy(buf, path, n + 1);
    for (i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == 0) {
            char c = buf[i];
            buf[i] = 0;
            if (vkc_mkdir(buf) != 0 && errno != EEXIST) {
                /* A drive ("C:") or a part we may not create but can enter. */
                if (stat(buf, &st) != 0)
                    return 0;
            }
            buf[i] = c;
        }
    }
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

int vkc_open(const char *dir, uint64_t seed)
{
    char sub[1100];

    memset(&s, 0, sizeof s);
    if (!dir || !*dir || strlen(dir) >= sizeof s.dir)
        return 0;
    snprintf(sub, sizeof sub, "%s/spirv", dir);
    if (!make_dirs(sub))
        return 0;
    snprintf(s.dir, sizeof s.dir, "%s", dir);
    s.seed = seed;
    s.on = 1;
    return 1;
}

void vkc_close(void) { memset(&s, 0, sizeof s); }
int vkc_enabled(void) { return s.on; }

/* The whole file, or NULL. */
static void *read_file(const char *path, size_t max, size_t *size)
{
    FILE *f = fopen(path, "rb");
    long n;
    void *p = NULL;

    *size = 0;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0 && (size_t)n <= max
            && fseek(f, 0, SEEK_SET) == 0 && (p = malloc((size_t)n)) != NULL) {
        if (fread(p, 1, (size_t)n, f) == (size_t)n) {
            *size = (size_t)n;
        } else {
            free(p);
            p = NULL;
        }
    }
    fclose(f);
    return p;
}

/* Header and payload to a temporary file, then renamed over `path`. */
static int write_file(const char *path, const void *head, size_t hn, const void *body, size_t bn)
{
    char tmp[1200];
    FILE *f;
    int ok;

    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)vkc_getpid());
    f = fopen(tmp, "wb");
    if (!f)
        return 0;
    ok = fwrite(head, 1, hn, f) == hn && fwrite(body, 1, bn, f) == bn;
    ok = (fclose(f) == 0) && ok;
#ifdef _WIN32
    if (ok)
        remove(path);       /* rename does not replace there */
#endif
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

static uint64_t spv_key(const char *src, size_t len, int stage)
{
    uint8_t st = (uint8_t)(stage ? 1 : 0);
    return vkc_hash64(src, len, vkc_hash64(&st, 1, s.seed));
}

static uint64_t spv_check(const char *src, size_t len)
{
    return vkc_hash64(src, len, s.seed ^ 0x9e3779b97f4a7c15ull);
}

static void spv_path(char *out, size_t n, uint64_t key, int stage)
{
    snprintf(out, n, "%s/spirv/%016llx.%s.spv", s.dir, (unsigned long long)key, stage ? "vs" : "ps");
}

uint32_t *vkc_spirv_load(const char *src, size_t len, int stage, size_t *bytes)
{
    char path[1200];
    SpvHeader h;
    uint8_t *file;
    uint32_t *code;
    size_t n;
    uint64_t key;

    *bytes = 0;
    if (!s.on || !src)
        return NULL;
    key = spv_key(src, len, stage);
    spv_path(path, sizeof path, key, stage);
    file = (uint8_t *)read_file(path, sizeof h + SPV_MAX, &n);
    if (!file)
        return NULL;
    if (n < sizeof h)
        goto bad;
    memcpy(&h, file, sizeof h);
    if (memcmp(h.magic, SPV_MAGIC, 8) || h.seed != s.seed || h.key != key
            || h.check != spv_check(src, len) || h.text_len != (uint64_t)len
            || h.stage != (uint32_t)(stage ? 1 : 0) || h.bytes < 20 || (h.bytes & 3)
            || n != sizeof h + h.bytes
            || h.payload != vkc_hash64(file + sizeof h, h.bytes, VKC_HASH_SEED))
        goto bad;
    code = (uint32_t *)malloc(h.bytes);
    if (!code)
        goto bad;
    memcpy(code, file + sizeof h, h.bytes);
    free(file);
    if (code[0] != SPIRV_WORD0) {
        free(code);
        return NULL;
    }
    *bytes = h.bytes;
    return code;
bad:
    free(file);
    return NULL;
}

int vkc_spirv_store(const char *src, size_t len, int stage, const uint32_t *code, size_t bytes)
{
    char path[1200];
    SpvHeader h;

    if (!s.on || !src || !code || bytes < 20 || (bytes & 3) || bytes > SPV_MAX || code[0] != SPIRV_WORD0)
        return 0;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, SPV_MAGIC, 8);
    h.seed = s.seed;
    h.key = spv_key(src, len, stage);
    h.check = spv_check(src, len);
    h.text_len = len;
    h.stage = stage ? 1u : 0u;
    h.bytes = (uint32_t)bytes;
    h.payload = vkc_hash64(code, bytes, VKC_HASH_SEED);
    spv_path(path, sizeof path, h.key, stage);
    return write_file(path, &h, sizeof h, code, bytes);
}

/* VkPipelineCacheHeaderVersionOne: headerSize, headerVersion (1), vendorID,
 * deviceID, pipelineCacheUUID[16]; 32 bytes, little-endian as the host is. */
int vkc_pipeline_header_ok(const VkcDevice *dev, const void *data, size_t size)
{
    uint32_t w[4];

    if (!dev || !data || size < 32)
        return 0;
    memcpy(w, data, sizeof w);
    return w[0] >= 32 && w[0] <= size && w[1] == 1u
        && w[2] == dev->vendor_id && w[3] == dev->device_id
        && !memcmp((const uint8_t *)data + 16, dev->uuid, 16);
}

static void pipe_path(char *out, size_t n)
{
    snprintf(out, n, "%s/pipelines.bin", s.dir);
}

static int same_device(const VkcDevice *a, const VkcDevice *b)
{
    return a->vendor_id == b->vendor_id && a->device_id == b->device_id
        && a->driver_version == b->driver_version && !memcmp(a->uuid, b->uuid, 16);
}

void *vkc_pipeline_load(const VkcDevice *dev, size_t *size)
{
    char path[1200];
    PipeHeader h;
    uint8_t *file, *data;
    size_t n;

    *size = 0;
    if (!s.on || !dev)
        return NULL;
    pipe_path(path, sizeof path);
    file = (uint8_t *)read_file(path, sizeof h + PIPE_MAX, &n);
    if (!file)
        return NULL;
    memcpy(&h, file, n < sizeof h ? n : sizeof h);
    if (n < sizeof h || memcmp(h.magic, PIPE_MAGIC, 8) || h.format != PIPE_FORMAT
            || !same_device(&h.dev, dev) || n != sizeof h + h.bytes
            || h.payload != vkc_hash64(file + sizeof h, h.bytes, VKC_HASH_SEED)
            || !vkc_pipeline_header_ok(dev, file + sizeof h, h.bytes)) {
        free(file);
        return NULL;
    }
    data = (uint8_t *)malloc(h.bytes);
    if (data)
        memcpy(data, file + sizeof h, h.bytes);
    free(file);
    if (data)
        *size = h.bytes;
    return data;
}

int vkc_pipeline_store(const VkcDevice *dev, const void *data, size_t size)
{
    char path[1200];
    PipeHeader h;

    if (!s.on || size > PIPE_MAX || !vkc_pipeline_header_ok(dev, data, size))
        return 0;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, PIPE_MAGIC, 8);
    h.format = PIPE_FORMAT;
    h.bytes = (uint32_t)size;
    h.payload = vkc_hash64(data, size, VKC_HASH_SEED);
    h.dev = *dev;
    pipe_path(path, sizeof path);
    return write_file(path, &h, sizeof h, data, size);
}
