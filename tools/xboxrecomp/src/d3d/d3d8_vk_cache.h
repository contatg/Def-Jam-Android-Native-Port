/*
 * d3d8_vk_cache.h -- what the Vulkan device keeps on disk between runs.
 *
 * Two things, both under one folder the host chooses (d3d8_vk_set_cache_dir):
 *
 *   spirv/<key>.spv    the SPIR-V shaderc made from one HLSL text, so a shader
 *                      seen in an earlier run is not compiled again. The key is
 *                      a hash of the text, the stage and a version seed that
 *                      names the translator and the compiler options; anything
 *                      that changes the SPIR-V for the same text changes the
 *                      seed, and an entry made under another seed is never found.
 *   pipelines.bin      the driver's VkPipelineCache data, read when the device
 *                      is made and written when it has grown.
 *
 * Every file carries its own header: a magic, the key or device it belongs to,
 * the payload size and a hash of the payload. A file that is short, damaged,
 * from another version or for another device is ignored (and is replaced the
 * next time that entry is written), never handed to the driver. Pipeline data
 * is also checked against the header Vulkan defines for it
 * (VkPipelineCacheHeaderVersionOne) before the driver sees it.
 *
 * Files are written to a temporary name and renamed over the old one, so a run
 * killed while writing leaves the previous file or none, and two processes
 * sharing a folder do not see each other's half-written files.
 *
 * This file knows nothing about Vulkan beyond the header layout, so its tests
 * need no device.
 */
#ifndef XBOXRECOMP_D3D8_VK_CACHE_H
#define XBOXRECOMP_D3D8_VK_CACHE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VKC_HASH_SEED 0xcbf29ce484222325ull

/* 64-bit FNV-1a over n bytes, continuing from h (VKC_HASH_SEED to start). */
uint64_t vkc_hash64(const void *p, size_t n, uint64_t h);

/* The device a pipeline cache was made on, as VkPhysicalDeviceProperties gives it. */
typedef struct VkcDevice {
    uint32_t vendor_id;
    uint32_t device_id;
    uint32_t driver_version;
    uint8_t  uuid[16];          /* pipelineCacheUUID */
} VkcDevice;

/* Use `dir` (made, with its parents, if missing) under version seed `seed`.
 * Returns 1 when the folder can be used; 0 leaves the cache off, which every
 * call below then answers as a miss. NULL or "" turns it off. */
int  vkc_open(const char *dir, uint64_t seed);
void vkc_close(void);
int  vkc_enabled(void);

/* SPIR-V for `len` bytes of HLSL text `src`; stage 1 vertex, 0 pixel. Returns
 * the words (free() them) and their size in bytes, or NULL on a miss or an
 * entry that does not check out. */
uint32_t *vkc_spirv_load(const char *src, size_t len, int stage, size_t *bytes);
/* Keep it. Returns 1 when written. */
int vkc_spirv_store(const char *src, size_t len, int stage, const uint32_t *code, size_t bytes);

/* 1 when `data` starts with a VkPipelineCacheHeaderVersionOne for `dev`. */
int vkc_pipeline_header_ok(const VkcDevice *dev, const void *data, size_t size);
/* The saved pipeline cache for `dev` (free() it), or NULL. */
void *vkc_pipeline_load(const VkcDevice *dev, size_t *size);
/* Keep it. Returns 1 when written; data that is not for `dev` is not written. */
int vkc_pipeline_store(const VkcDevice *dev, const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VK_CACHE_H */
