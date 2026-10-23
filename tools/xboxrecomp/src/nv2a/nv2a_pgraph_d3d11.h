/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Intercepts NV2A push buffer method calls and translates them into
 * D3D8→D3D11 rendering commands. This is the core of the GPU translation
 * layer for Xbox static recompilation.
 *
 * The push buffer contains NV2A Kelvin (NV097) methods:
 *   - Surface/viewport setup → D3D11 render target + viewport
 *   - Render state (blend, depth, cull) → D3D11 state objects
 *   - Begin/End draw + Inline vertex data → D3D11 DrawPrimitiveUP
 *   - Texture binding → D3D11 shader resource views
 *   - Clear commands → D3D11 ClearRenderTargetView
 *
 * Vertex formats observed in menus:
 *   5 dwords per vertex: float X, float Y, float U, float V, D3DCOLOR
 *   Drawn as TRIANGLE_STRIP (mode 6)
 *
 * This module is designed to be reusable across Xbox recompilation projects.
 * See: https://github.com/sp00nznet/xboxrecomp
 */

#ifndef NV2A_PGRAPH_D3D11_H
#define NV2A_PGRAPH_D3D11_H

#include <stdint.h>
#include "texture_pack.h"
#include "../kernel/nv2a_combiner.h"

/* Initialize the PGRAPH→D3D11 translator. Call after D3D11 device is created. */
void pgraph_d3d11_init(void);

/* Shut down and release resources. */
void pgraph_d3d11_shutdown(void);
int pgraph_d3d11_try_texture_pack(const RecompTextureSource *source,
                                RecompTextureSample sample, void *user);

/* Process an NV2A PGRAPH method call. Called from push buffer parser.
 * Returns 1 if handled, 0 if unhandled (caller should log/ignore). */
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param);
#if defined(__ANDROID__)
/* ARRAY_ELEMENT16/32 are consumed by the push-buffer executor. This
 * translator only counts them as unhandled; preserve those exact counters
 * when the executor appends a complete index packet in one operation. */
void pgraph_d3d11_note_index_words(uint32_t words);
#endif

/* Flush any pending draw commands (call at end of frame). */
void pgraph_d3d11_flush(void);

/* Draw vertices the caller has already fetched and placed in screen space.
 *
 * The inline path above covers titles that carry vertex data in the command
 * stream. Most do not: they point the GPU at attribute arrays in memory and
 * draw by index, and resolving those addresses is the command decoder's job,
 * not the renderer's -- it already tracks the DMA objects they live in. So the
 * decoder fetches, and this draws what it produced.
 *
 * `nv2a_prim_mode` is the NV2A primitive, the value SET_BEGIN_END carries,
 * not a Direct3D one. Quads and quad strips are expanded here, because the
 * hardware has them and Direct3D does not. */
typedef struct {
    float    x, y;      /* screen space, as the title computed it */
    float    z;         /* depth, 0..1 (the title's depth-buffer units scaled) */
    float    u, v;      /* texture coordinates, stage 0 */
    uint32_t color;     /* diffuse, ARGB */
    float    w;         /* clip-space w (1 when there is none); x, y, z are
                         * already divided by it, the Xbox vertex shader
                         * epilogue's screen space */
} PgraphReadyVertex;

void pgraph_d3d11_draw_ready(uint32_t nv2a_prim_mode,
                             const PgraphReadyVertex *verts, uint32_t count);

/* Bind the texture the title has programmed, uploading it the first time it is
 * seen. `data` points at the texture in host memory, already resolved by the
 * caller, and `nv2a_format` is the NV2A colour field, which is the Xbox
 * D3DFORMAT code. A zero width or height draws untextured. Cached on the guest
 * address and format, so a texture bound every frame is uploaded once, and
 * re-read now and then so one the title rewrites in place (a movie frame) is
 * uploaded again. `data` may be NULL for a key already uploaded, to bind it
 * without re-reading: for a texture the caller converted itself.
 *
 * Returns 1 if a texture is bound, 0 if the draw will be untextured -- in
 * particular for a NULL `data` whose key is not (or no longer) cached, which
 * the caller answers by converting and passing the data. */
int pgraph_d3d11_set_texture_ready(uint32_t guest_addr, const void *data,
                                   uint32_t width, uint32_t height,
                                   uint32_t nv2a_format);

/* The same, for a caller that knows its converted data changed: an entry
 * already cached under the key is refilled now, not at the next periodic
 * re-read. */
int pgraph_d3d11_replace_texture_ready(uint32_t guest_addr, const void *data,
                                       uint32_t width, uint32_t height,
                                       uint32_t nv2a_format);

/* The bound texture's addressing, as the title set it (NV097_SET_TEXTURE_
 * ADDRESS, per axis: 1 wrap, 2 mirror, 3 clamp to edge, 4 border, 5 clamp).
 * Call after binding: binding resets both axes to clamp. */
void pgraph_d3d11_set_texture_address(uint32_t nv2a_u, uint32_t nv2a_v);
void pgraph_d3d11_set_texture_filter(uint32_t nv2a_filter);

/* A batch drawn with the current NV2A vertex program run on the GPU (M4e):
 * `verts` holds, per vertex, one float4 for each input the program reads
 * (bit n of `used` = v<n>, ascending); `idx` is a triangle list into them.
 * tex_*_scale multiply texture coordinate 0 (1 / a linear texture's texels).
 * Returns 1 when drawn (or when the batch writes nothing), -1 when this
 * program must take the CPU path. */
/* Texture stage for the next set_texture_ready/set_texture_address (0..3),
 * and a stage emptied. */
void pgraph_d3d11_bind_stage(int stage);
void pgraph_d3d11_unbind_stage(int stage);
void pgraph_d3d11_set_surface_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void pgraph_d3d11_set_point_mode(int on, float size);
/* Widescreen: clip-space x of the next draws becomes x * kx + bx (1, 0 = unchanged). */
void pgraph_d3d11_set_placement(float kx, float bx);
/* Widescreen 2D placement policy (nv2a_pb_exec.c): k = title aspect / canvas aspect
 * (1 = off); hud_reach = how far towards the screen edge anchored HUD pieces go
 * (1 = the edge). The heartbeat, once per game tick, says a fight HUD is up. */
void nv2a_ws_configure(float k, float hud_reach);
void nv2a_ws_hud_heartbeat(int bottom_rows);
void nv2a_ws_set_side_fill(int on);

/* The title's register combiners for the next draws (see the .c). */
void pgraph_d3d11_set_combiners(const uint32_t *a, const uint32_t *b, const uint32_t *c,
                                uint32_t final_c0, uint32_t final_c1);

/* Whether the current program can take the GPU path. */
int pgraph_d3d11_program_usable(void);

int pgraph_d3d11_draw_program(const float *verts, uint32_t nverts, uint32_t used,
                              const uint16_t *idx, uint32_t nidx,
                              float tex_u_scale, float tex_v_scale);

/* Set chyron scroll: pass frame counter to animate, 0 to disable.
 * Applies horizontal scroll offset to vertices in the chyron Y band. */
void pgraph_d3d11_set_chyron_scroll(uint32_t frame);

/* The host supplies presentation for its swap chain. */
#define RECOMP_PGRAPH_FRAME_END_CALLBACK 1
void pgraph_d3d11_set_frame_end_callback(void (*callback)(void));
void pgraph_d3d11_frame_end(void);
void pgraph_d3d11_set_combiner_state(const Nv2aCombiner *state);

/* Statistics */
typedef struct {
    uint32_t frames;
    uint32_t draw_calls;
    uint32_t vertices_submitted;
    uint32_t methods_handled;
    uint32_t methods_ignored;
    uint32_t clears;
    /* The texture cache (appended; earlier fields unchanged). Running totals: textures
     * uploaded, new or changed in place, their bytes, and evictions from a full cache.
     * Now: the entries the cache holds and their bytes. */
    uint32_t textures_uploaded;
    uint32_t texture_evictions;
    uint64_t texture_upload_bytes;
    uint32_t textures_cached;
    uint64_t texture_cache_bytes;
} PgraphD3D11Stats;

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out);

#endif /* NV2A_PGRAPH_D3D11_H */
