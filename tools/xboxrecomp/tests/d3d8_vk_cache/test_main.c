/*
 * d3d8_vk_cache fixture: the Vulkan device's on-disk shader and pipeline cache
 * (src/d3d/d3d8_vk_cache.h) in a fresh temporary folder.
 *
 * SPIR-V: what is stored comes back for the same text and stage, and nothing
 * comes back for another text, another stage or another version seed. A file
 * that is damaged (a flipped byte, cut short, another magic, not SPIR-V, an
 * empty file) is a miss, and storing again replaces it.
 *
 * Pipeline cache: the VkPipelineCacheHeaderVersionOne check takes only the
 * device's own header; a file for another device or driver, or damaged, loads
 * nothing; data whose header names another device is not written.
 *
 * Also: with no folder everything is a miss and nothing is written, a missing
 * folder is made with its parents, and no temporary file is left behind.
 */
#ifdef _WIN32
#include <stdio.h>
int main(void) { puts("SKIP: the Vulkan device is not built on Windows"); return 77; }
#else

#if !defined(__APPLE__)
#define _XOPEN_SOURCE 700     /* nftw, mkdtemp, truncate; macOS has them by default */
#endif
#include "d3d8_vk_cache.h"

#include <dirent.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

static char g_root[512], g_dir[600];

static const char VS[] = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
static const char PS[] = "float4 main() : SV_Target { return float4(1, 0, 0, 1); }";

/* A plausible SPIR-V module: the magic and a few words. */
static uint32_t g_code[16];
static void make_code(uint32_t salt)
{
    uint32_t i;
    g_code[0] = 0x07230203u;
    g_code[1] = 0x00010000u;
    for (i = 2; i < 16; i++)
        g_code[i] = salt * 2654435761u + i;
}

static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p);
}

/* Each file in dir: its full path to fn. Returns how many. */
static int each_file(const char *dir, void (*fn)(const char *path))
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char path[1024];
    int n = 0;
    if (!d)
        return 0;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (fn)
            fn(path);
        n++;
    }
    closedir(d);
    return n;
}

static int g_tmp_seen;
static void note_tmp(const char *path)
{
    if (strstr(path, ".tmp"))
        g_tmp_seen++;
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static void flip_byte(const char *path, long at)
{
    FILE *f = fopen(path, "r+b");
    int c;
    if (!f)
        return;
    if (at < 0)
        at += file_size(path);
    fseek(f, at, SEEK_SET);
    c = fgetc(f);
    fseek(f, at, SEEK_SET);
    fputc(c ^ 0x5A, f);
    fclose(f);
}

static void cut_to(const char *path, long size) { if (truncate(path, size) != 0) perror(path); }
static void cut_half(const char *path) { cut_to(path, file_size(path) / 2); }
static void cut_empty(const char *path) { cut_to(path, 0); }
static void flip_magic(const char *path) { flip_byte(path, 0); }
static void flip_last(const char *path) { flip_byte(path, -1); }
static void add_byte(const char *path)
{
    FILE *f = fopen(path, "ab");
    if (f) { fputc(0, f); fclose(f); }
}

static int spv_hit(const char *src, int stage, uint32_t salt)
{
    size_t bytes = 0;
    uint32_t *code = vkc_spirv_load(src, strlen(src), stage, &bytes);
    int ok = code && bytes == sizeof g_code;
    if (ok) {
        make_code(salt);
        ok = !memcmp(code, g_code, sizeof g_code);
    }
    free(code);
    return ok;
}

static int spv_miss(const char *src, int stage)
{
    size_t bytes = 1234;
    uint32_t *code = vkc_spirv_load(src, strlen(src), stage, &bytes);
    int miss = !code && bytes == 0;
    free(code);
    return miss;
}

static void test_off(void)
{
    VkcDevice dev;
    size_t n = 1;

    memset(&dev, 0, sizeof dev);
    CHECK(!vkc_open(NULL, 1), "NULL folder opens");
    CHECK(!vkc_open("", 1), "empty folder opens");
    CHECK(!vkc_enabled(), "enabled with no folder");
    make_code(1);
    CHECK(!vkc_spirv_store(VS, strlen(VS), 1, g_code, sizeof g_code), "stored with no folder");
    CHECK(spv_miss(VS, 1), "hit with no folder");
    CHECK(vkc_pipeline_load(&dev, &n) == NULL && n == 0, "pipeline data with no folder");
}

static void test_spirv(void)
{
    char spv[700];
    void (*damage[])(const char *) = { flip_magic, flip_last, cut_half, cut_empty, add_byte };
    const char *what[] = { "a flipped magic", "a flipped last byte", "a file cut in half",
                           "an empty file", "a byte too many" };
    size_t i;

    snprintf(spv, sizeof spv, "%s/spirv", g_dir);
    CHECK(vkc_open(g_dir, 0x1111), "a missing nested folder is not made");
    CHECK(vkc_enabled(), "not enabled after open");
    CHECK(file_size(spv) >= 0, "spirv/ not made");

    CHECK(spv_miss(VS, 1), "hit before anything was stored");
    make_code(1);
    CHECK(vkc_spirv_store(VS, strlen(VS), 1, g_code, sizeof g_code), "vertex store failed");
    make_code(2);
    CHECK(vkc_spirv_store(PS, strlen(PS), 0, g_code, sizeof g_code), "pixel store failed");
    CHECK(spv_hit(VS, 1, 1), "vertex SPIR-V does not come back");
    CHECK(spv_hit(PS, 0, 2), "pixel SPIR-V does not come back");
    CHECK(spv_miss(VS, 0), "the vertex text as a pixel shader hits");
    CHECK(spv_miss(PS, 1), "the pixel text as a vertex shader hits");
    CHECK(spv_miss("float4 main() : SV_Target { return 0; }", 0), "other text hits");
    CHECK(each_file(spv, NULL) == 2, "expected two files in spirv/, found %d", each_file(spv, NULL));

    /* Not SPIR-V, or not whole words: not written. */
    make_code(3);
    g_code[0] = 0xDEADBEEFu;
    CHECK(!vkc_spirv_store("x", 1, 0, g_code, sizeof g_code), "stored without the SPIR-V magic");
    make_code(3);
    CHECK(!vkc_spirv_store("x", 1, 0, g_code, sizeof g_code - 2), "stored a size that is not whole words");
    CHECK(!vkc_spirv_store("x", 1, 0, g_code, 16), "stored less than a SPIR-V header");

    /* Another version seed: the same text is a miss; back to the first, a hit. */
    CHECK(vkc_open(g_dir, 0x2222), "reopen failed");
    CHECK(spv_miss(VS, 1), "an entry from another seed hits");
    CHECK(vkc_open(g_dir, 0x1111), "reopen failed");
    CHECK(spv_hit(VS, 1, 1), "the first seed's entry is gone");

    /* Each kind of damage to every file: a miss, then a store mends it. */
    for (i = 0; i < sizeof damage / sizeof damage[0]; i++) {
        each_file(spv, damage[i]);
        CHECK(spv_miss(VS, 1), "vertex SPIR-V hits after %s", what[i]);
        CHECK(spv_miss(PS, 0), "pixel SPIR-V hits after %s", what[i]);
        make_code(1);
        CHECK(vkc_spirv_store(VS, strlen(VS), 1, g_code, sizeof g_code), "restore failed");
        make_code(2);
        CHECK(vkc_spirv_store(PS, strlen(PS), 0, g_code, sizeof g_code), "restore failed");
        CHECK(spv_hit(VS, 1, 1) && spv_hit(PS, 0, 2), "store did not replace %s", what[i]);
    }
    g_tmp_seen = 0;
    each_file(spv, note_tmp);
    CHECK(g_tmp_seen == 0, "%d temporary files left in spirv/", g_tmp_seen);
}

/* A pipeline cache as a driver hands it out: its header, then its own bytes. */
static uint8_t g_pipe[256];
static void make_pipe(const VkcDevice *dev, uint32_t header_size, uint32_t version)
{
    uint32_t w[4] = { header_size, version, dev->vendor_id, dev->device_id };
    size_t i;
    memcpy(g_pipe, w, sizeof w);
    memcpy(g_pipe + 16, dev->uuid, 16);
    for (i = 32; i < sizeof g_pipe; i++)
        g_pipe[i] = (uint8_t)(i * 7);
}

static void test_pipeline(void)
{
    VkcDevice dev, other;
    char path[700];
    void *data;
    size_t n;
    int i;

    memset(&dev, 0, sizeof dev);
    dev.vendor_id = 0x106B;
    dev.device_id = 0x1A020209;
    dev.driver_version = 10302;
    for (i = 0; i < 16; i++)
        dev.uuid[i] = (uint8_t)(0xA0 + i);
    snprintf(path, sizeof path, "%s/pipelines.bin", g_dir);

    /* The Vulkan header check. */
    make_pipe(&dev, 32, 1);
    CHECK(vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "a good header is refused");
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, 31), "a short header passes");
    CHECK(!vkc_pipeline_header_ok(&dev, NULL, sizeof g_pipe), "no data passes");
    make_pipe(&dev, 32, 2);
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "header version 2 passes");
    make_pipe(&dev, 16, 1);
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "a header size under 32 passes");
    make_pipe(&dev, 4096, 1);
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "a header longer than the data passes");
    other = dev;
    other.vendor_id ^= 1;
    make_pipe(&other, 32, 1);
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "another vendor passes");
    other = dev;
    other.device_id ^= 1;
    make_pipe(&other, 32, 1);
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "another device passes");
    other = dev;
    other.uuid[15] ^= 1;
    make_pipe(&other, 32, 1);
    CHECK(!vkc_pipeline_header_ok(&dev, g_pipe, sizeof g_pipe), "another cache UUID passes");
    CHECK(!vkc_pipeline_store(&dev, g_pipe, sizeof g_pipe), "data for another UUID was written");
    CHECK(file_size(path) < 0, "pipelines.bin exists after a refused store");

    /* Round trip. */
    CHECK(vkc_open(g_dir, 0x1111), "reopen failed");
    n = 1;
    CHECK(vkc_pipeline_load(&dev, &n) == NULL && n == 0, "pipeline data before any was stored");
    make_pipe(&dev, 32, 1);
    CHECK(vkc_pipeline_store(&dev, g_pipe, sizeof g_pipe), "pipeline store failed");
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data && n == sizeof g_pipe && !memcmp(data, g_pipe, n), "pipeline data does not come back");
    free(data);

    /* The pipeline cache does not depend on the shader seed. */
    CHECK(vkc_open(g_dir, 0x2222), "reopen failed");
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data != NULL, "pipeline data lost with another shader seed");
    free(data);

    /* Another driver version, same UUID: not loaded. */
    other = dev;
    other.driver_version++;
    data = vkc_pipeline_load(&other, &n);
    CHECK(data == NULL && n == 0, "pipeline data loads for another driver version");
    free(data);

    /* Damage. */
    flip_last(path);
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data == NULL, "a flipped payload byte loads");
    free(data);
    CHECK(vkc_pipeline_store(&dev, g_pipe, sizeof g_pipe), "pipeline store failed");
    cut_half(path);
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data == NULL, "a file cut in half loads");
    free(data);
    cut_empty(path);
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data == NULL, "an empty file loads");
    free(data);
    CHECK(vkc_pipeline_store(&dev, g_pipe, sizeof g_pipe), "pipeline store failed");
    flip_magic(path);
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data == NULL, "a flipped magic loads");
    free(data);
    CHECK(vkc_pipeline_store(&dev, g_pipe, sizeof g_pipe), "store did not replace a damaged file");
    data = vkc_pipeline_load(&dev, &n);
    CHECK(data != NULL, "pipeline data does not come back after a repair");
    free(data);

    g_tmp_seen = 0;
    each_file(g_dir, note_tmp);
    CHECK(g_tmp_seen == 0, "%d temporary files left", g_tmp_seen);
}

int main(void)
{
    const char *tmp = getenv("TMPDIR");

    snprintf(g_root, sizeof g_root, "%s/d3d8_vk_cache.XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(g_root)) {
        puts("SKIP: no temporary folder");
        return 77;
    }
    snprintf(g_dir, sizeof g_dir, "%s/one/two/vulkan", g_root);

    CHECK(vkc_hash64("abc", 3, VKC_HASH_SEED) == 0xe71fa2190541574bull, "FNV-1a 64 of \"abc\" is wrong");
    test_off();
    test_spirv();
    test_pipeline();
    vkc_close();
    CHECK(!vkc_enabled(), "enabled after close");

    nftw(g_root, rm_one, 16, FTW_DEPTH | FTW_PHYS);
    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("d3d8_vk_cache: all checks passed");
    return 0;
}
#endif
