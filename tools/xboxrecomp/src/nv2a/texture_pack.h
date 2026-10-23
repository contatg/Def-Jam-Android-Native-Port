#ifndef RECOMP_TEXTURE_PACK_H
#define RECOMP_TEXTURE_PACK_H
#include <stdint.h>
#include "../d3d/d3d8_xbox.h"

/* Source dimensions/stride remain guest properties. Replacements never alter them. */
typedef struct RecompTextureSource {
    uint32_t address, format, width, height, row_texels;
    const void *data;
    uint32_t bytes, stride, row_bytes, rows;
    const void *palette;
    uint32_t palette_entries;
} RecompTextureSource;
typedef int (*RecompTextureSample)(void *user, uint32_t x, uint32_t y, uint32_t *argb);
int texture_pack_active(void);
int texture_pack_hash(const RecompTextureSource *source, char out[65]);
int texture_pack_bind(IDirect3DDevice8 *device, unsigned stage,
                      const RecompTextureSource *source, RecompTextureSample sample, void *user);
void texture_pack_shutdown(IDirect3DDevice8 *device);
/* Replacements resident now and the budget they share (cache_mb), in bytes; 0 without a pack. */
void texture_pack_stats(uint64_t *resident_bytes, uint64_t *budget_bytes);
#endif
