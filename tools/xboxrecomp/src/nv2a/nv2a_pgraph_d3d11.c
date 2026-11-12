/*
 * NV2A PGRAPH → D3D11 Translator
 *
 * Translates NV2A push buffer methods into D3D8→D3D11 rendering calls.
 * Designed for Xbox static recompilation (xboxrecomp toolkit).
 *
 * Menu rendering profile (captured from xemu):
 *   - INLINE_ARRAY with 5-dword vertices (X, Y, U, V, Color)
 *   - TRIANGLE_STRIP topology
 *   - ~448 vertices per frame (~89 quads)
 *   - Textured 2D elements in 640×480 screen space
 */

#include "nv2a_pgraph_d3d11.h"
#include "nv2a_regs.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* D3D8 device — we include the full header for COM vtable access */
#include "../d3d/d3d8_xbox.h"
#include "../d3d/d3d8_swizzle.h"   /* unswizzle and the format predicates */
extern IDirect3DDevice8 *xbox_GetD3DDevice(void);

/* Global.txd texture lookup */
/* Game-specific texture lookup - only available when GAME_HAS_FONT_ATLAS is defined */
#ifdef GAME_HAS_FONT_ATLAS
typedef struct { char name[24]; IDirect3DTexture8 *texture; uint32_t width, height, format; } TXD_Entry;
typedef struct { TXD_Entry entries[512]; int count; } TXD_Dict;
extern TXD_Dict g_global_txd;
extern int g_textures_loaded;
extern IDirect3DTexture8 *txd_find(const TXD_Dict *dict, const char *name);
#else
static int g_textures_loaded = 0;
#endif

/* Font atlas DXT5 data - reference-title frontend data. Only compiled in when
 * GAME_HAS_FONT_ATLAS is defined for a title whose HUD needs this mapping. */
#ifdef GAME_HAS_FONT_ATLAS
#include "font_atlas_data.h"
#endif

/* Create a D3D8 texture from raw DXT5 data */
static IDirect3DTexture8 *create_dxt5_texture(IDirect3DDevice8 *dev,
    uint32_t width, uint32_t height, const void *dxt5_data, uint32_t data_size)
{
    IDirect3DTexture8 *tex = NULL;
    /* D3DFMT_DXT5 = 0x35545844 ('DXT5') on Xbox, mapped to DXGI_FORMAT_BC3 in our layer.
     * Our d3d8 layer uses format code 0x0F for DXT5. */
    HRESULT hr = dev->lpVtbl->CreateTexture(dev, width, height, 1,
        0 /*Usage*/, 0x0F /*DXT5*/, 0 /*D3DPOOL_DEFAULT*/, &tex);
    if (hr != 0 || !tex) {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to create font atlas texture: hr=0x%08X\n", hr);
        return NULL;
    }

    /* Lock and fill with DXT5 data */
    D3DLOCKED_RECT lr = {0};
    hr = tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0);
    if (hr == 0 && lr.pBits) {
        memcpy(lr.pBits, dxt5_data, data_size);
        tex->lpVtbl->UnlockRect(tex, 0);
        fprintf(stderr, "[PGRAPH-D3D11] Created font atlas: %ux%u DXT5 (%u bytes)\n",
                width, height, data_size);
    } else {
        fprintf(stderr, "[PGRAPH-D3D11] Failed to lock font atlas: hr=0x%08X\n", hr);
    }
    return tex;
}

/* Method numbers come from nv2a_regs.h, included above.
 *
 * There was a local copy of them here, and seven of its eighteen values
 * were wrong: the whole clear block (CLEAR_SURFACE, SET_COLOR_CLEAR_VALUE
 * and both clear rectangles), plus depth test, cull face and shade mode.
 * Because the copy came after the include it won every lookup, so those
 * cases matched method numbers the hardware never sends and silently did
 * nothing. The visible symptom was a translator reporting millions of
 * methods and zero clears against a title that clears every frame.
 *
 * Keeping one definition is the fix. A local subset of a register header
 * is a copy that cannot be checked, and this one had drifted.
 */

/* NV2A draw modes → D3D primitive types */
static int nv2a_draw_mode_to_d3d(uint32_t mode) {
    switch (mode) {
        case 1:  return D3DPT_POINTLIST;
        case 2:  return D3DPT_LINELIST;
        case 3:  return D3DPT_LINESTRIP;  /* LINE_LOOP → LINE_STRIP */
        case 4:  return D3DPT_LINESTRIP;
        case 5:  return D3DPT_TRIANGLELIST;
        case 6:  return D3DPT_TRIANGLESTRIP;
        case 7:  return D3DPT_TRIANGLEFAN;
        case 8:  return D3DPT_TRIANGLELIST; /* QUADS → TRI_LIST (needs conversion) */
        default: return D3DPT_TRIANGLELIST;
    }
}

/* NV2A blend factors → D3D blend */
static uint32_t nv2a_blend_to_d3d(uint32_t nv) {
    /* The NV2A takes OpenGL's blend-factor enums. */
    switch (nv) {
        case 0x0000: return D3DBLEND_ZERO;
        case 0x0001: return D3DBLEND_ONE;
        case 0x0300: return D3DBLEND_SRCCOLOR;
        case 0x0301: return D3DBLEND_INVSRCCOLOR;
        case 0x0302: return D3DBLEND_SRCALPHA;
        case 0x0303: return D3DBLEND_INVSRCALPHA;
        case 0x0304: return D3DBLEND_DESTALPHA;
        case 0x0305: return D3DBLEND_INVDESTALPHA;
        case 0x0306: return D3DBLEND_DESTCOLOR;
        case 0x0307: return D3DBLEND_INVDESTCOLOR;
        case 0x0308: return D3DBLEND_SRCALPHASAT;
        case 0x8001: return D3DBLEND_CONSTANTCOLOR;
        case 0x8002: return D3DBLEND_INVCONSTANTCOLOR;
        case 0x8003: return D3DBLEND_CONSTANTALPHA;
        case 0x8004: return D3DBLEND_INVCONSTANTALPHA;
        default:     return D3DBLEND_ONE;
    }
}

/* NV2A blend equations are OpenGL's too; D3DBLENDOP numbers them 1..5
 * (add, subtract, reverse subtract, min, max). The signed variants
 * (0xF005, 0xF006) have no counterpart and fall back to their unsigned ones. */
static uint32_t nv2a_blend_eq_to_d3d(uint32_t nv)
{
    switch (nv) {
        case 0x800A: return 2;              /* FUNC_SUBTRACT */
        case 0x800B: case 0xF005: return 3; /* FUNC_REVERSE_SUBTRACT */
        case 0x8007: return 4;              /* MIN */
        case 0x8008: return 5;              /* MAX */
        default:     return 1;              /* FUNC_ADD (0x8006), ADD_SIGNED */
    }
}

/* NV2A comparison functions are OpenGL's (0x200 NEVER .. 0x207 ALWAYS), and
 * D3D's run in the same order from 1. */
static uint32_t nv2a_cmp_to_d3d(uint32_t nv)
{
    return (nv >= 0x200 && nv <= 0x207) ? nv - 0x200 + 1 : 8 /* ALWAYS */;
}

/* NV2A stencil operations are OpenGL's too; D3D numbers them 1..8. */
static uint32_t nv2a_stencil_op_to_d3d(uint32_t nv)
{
    switch (nv) {
    case 0x0000: return 2;              /* ZERO */
    case 0x1E01: return 3;              /* REPLACE */
    case 0x1E02: return 4;              /* INCR (saturate) */
    case 0x1E03: return 5;              /* DECR (saturate) */
    case 0x150A: return 6;              /* INVERT */
    case 0x8507: return 7;              /* INCR_WRAP */
    case 0x8508: return 8;              /* DECR_WRAP */
    default:     return 1;              /* KEEP (0x1E00) */
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Translator State
 * ══════════════════════════════════════════════════════════════════════ */

/* Inline vertex buffer - max 16K vertices per draw */
#define MAX_INLINE_VERTS 16384
#define INLINE_VERT_DWORDS 5  /* X, Y, U, V, Color */

/* RwIm2DVertex-compatible output vertex (28 bytes) */
typedef struct {
    float x, y, z, rhw;
    uint32_t color;
    float u, v;
} OutputVertex;

static struct {
    /* Draw state */
    int in_draw;           /* Between BEGIN and END */
    uint32_t draw_mode;    /* NV2A draw mode (0=end, 6=tristrip, etc.) */
    int d3d_prim_type;     /* Translated D3D prim type */

    /* Inline vertex accumulator */
    uint32_t inline_data[MAX_INLINE_VERTS * INLINE_VERT_DWORDS];
    uint32_t inline_count; /* Number of dwords accumulated */
    uint32_t vert_stride;  /* Dwords per vertex (auto-detected) */

    /* Clear state */
    uint32_t clear_color;
    uint32_t clear_rect_h;  /* (width << 16) | x */
    uint32_t clear_rect_v;  /* (height << 16) | y */

    /* Render state cache */
    int depth_test;
    uint32_t depth_func;   /* GL comparison, 0x200..0x207 */
    int depth_mask;        /* SET_DEPTH_MASK: depth writes */
    float zmax;            /* largest depth value of the zeta format */
    uint32_t zstencil_clear; /* SET_ZSTENCIL_CLEAR_VALUE, zeta-format packed */
    int blend_enable;
    uint32_t blend_sfactor;
    uint32_t blend_dfactor;
    uint32_t blend_color;       /* 0x034C, ARGB, for the CONSTANT_* factors */
    uint32_t blend_equation;    /* 0x0350 */
    int cull_enable;
    int alpha_test;
    uint32_t alpha_func, alpha_ref;
    uint32_t color_mask;
    /* Stencil (NV097 0x032C..0x0378), GL enums as the title sends them. */
    int stencil_enable;
    uint32_t stencil_func, stencil_ref, stencil_rmask, stencil_wmask;
    uint32_t stencil_fail, stencil_zfail, stencil_zpass;

    /* Viewport */
    float vp_offset[4];
    float vp_scale[4];
    uint32_t surface_clip_h;
    uint32_t surface_clip_v;

    /* Texture state per stage (4 stages) */
    struct {
        uint32_t offset;     /* NV2A VRAM offset (method 0x1B00) */
        uint32_t format;     /* Format register (method 0x1B04) */
        uint32_t control0;   /* Control0 register (method 0x1B08) */
        int enabled;         /* Decoded from control0 bit 30 */
    } tex[4];

    /* Cached texture pointers */
    void *menu_texture;           /* IDirect3DTexture8* from Global.txd */
    IDirect3DTexture8 *font_atlas; /* Created from captured DXT5 data */
    int texture_lookup_done;

    /* Stats */
    PgraphD3D11Stats stats;

    /* Chyron scroll */
    float chyron_scroll_offset;  /* Pixels to shift X for chyron text */

    /* Init flag */
    int initialized;
} g_pg;

/* ══════════════════════════════════════════════════════════════════════
 * Float/uint32 conversion
 * ══════════════════════════════════════════════════════════════════════ */
static float u2f(uint32_t u) {
    union { float f; uint32_t i; } x;
    x.i = u;
    return x.f;
}

/* ══════════════════════════════════════════════════════════════════════
 * Initialization
 * ══════════════════════════════════════════════════════════════════════ */

void pgraph_d3d11_init(void)
{
    memset(&g_pg, 0, sizeof(g_pg));
    g_pg.vert_stride = INLINE_VERT_DWORDS;  /* Default: 5 dwords per vertex */
    g_pg.clear_color = 0xFF000000;
    g_pg.color_mask = 0x01010101;
    g_pg.blend_sfactor = 0x0001;     /* GL defaults: ONE, ZERO, ALWAYS */
    g_pg.blend_dfactor = 0x0000;
    g_pg.blend_equation = 0x8006;
    g_pg.alpha_func = 0x0207;
    g_pg.depth_func = 0x0203;        /* D3D's default, LESSEQUAL */
    g_pg.depth_mask = 1;
    g_pg.zmax = 16777215.0f;         /* Z24S8 until the title says otherwise */
    g_pg.zstencil_clear = 0xFFFFFF00u;
    /* NV2A's reset values: test off, always pass, keep, all bits. */
    g_pg.stencil_func = 0x207;
    g_pg.stencil_rmask = g_pg.stencil_wmask = 0xFF;
    g_pg.stencil_fail = g_pg.stencil_zfail = g_pg.stencil_zpass = 0x1E00;
    g_pg.initialized = 1;

    fprintf(stderr, "[PGRAPH-D3D11] Translator initialized\n");
}

void pgraph_d3d11_shutdown(void)
{
    texture_pack_shutdown(xbox_GetD3DDevice());
    g_pg.initialized = 0;
    fprintf(stderr, "[PGRAPH-D3D11] Translator shut down (draws=%u, verts=%u)\n",
            g_pg.stats.draw_calls, g_pg.stats.vertices_submitted);
}

/* ══════════════════════════════════════════════════════════════════════
 * Draw Submission
 * ══════════════════════════════════════════════════════════════════════ */

static void submit_draw(void)
{
    if (g_pg.inline_count == 0 || g_pg.vert_stride == 0)
        return;
    {
        /* This path guesses a five-dword vertex and draws with a fixed 2D
         * state: no depth, always blended, no colour mask. The push-buffer
         * executor decodes inline arrays by the title's real vertex format and
         * draws them through draw_ready/draw_program with the title's state,
         * so drawing here as well paints the batch a second time, wrongly --
         * Def Jam's colour-masked magenta mask quad came out in full colour
         * over the Message Center. Kept only for a host that feeds methods
         * without the executor (RECOMP_TRANS_INLINE_DRAW=1). */
        static int legacy = -1;
        if (legacy < 0)
            legacy = getenv("RECOMP_TRANS_INLINE_DRAW") != NULL;
        if (!legacy) {
            g_pg.inline_count = 0;
            return;
        }
    }

    uint32_t num_verts = g_pg.inline_count / g_pg.vert_stride;
    if (num_verts < 3)
        return;

    const uint32_t *src = g_pg.inline_data;
    int actual_prim_type = g_pg.d3d_prim_type;
    uint32_t out_vert_count = num_verts;

    /* Handle QUADS (mode 8): convert to triangle list (6 verts per quad) */
    int is_quads = (g_pg.draw_mode == 8);
    uint32_t num_quads = is_quads ? (num_verts / 4) : 0;
    if (is_quads) {
        out_vert_count = num_quads * 6;  /* 2 triangles per quad */
        actual_prim_type = D3DPT_TRIANGLELIST;
    }

    /* Calculate primitive count */
    uint32_t prim_count = 0;
    switch (actual_prim_type) {
        case D3DPT_TRIANGLELIST:  prim_count = out_vert_count / 3; break;
        case D3DPT_TRIANGLESTRIP: prim_count = out_vert_count - 2; break;
        case D3DPT_TRIANGLEFAN:   prim_count = out_vert_count - 2; break;
        case D3DPT_LINELIST:      prim_count = out_vert_count / 2; break;
        case D3DPT_LINESTRIP:     prim_count = out_vert_count - 1; break;
        default: prim_count = out_vert_count / 3; break;
    }
    if (prim_count == 0)
        return;

    /* Convert inline vertices to OutputVertex (28 bytes) */
    OutputVertex *out = (OutputVertex *)_alloca(out_vert_count * sizeof(OutputVertex));

    /* Helper to convert one inline vertex */
    #define CONVERT_VERT(dst_idx, src_idx) do { \
        uint32_t _b = (src_idx) * g_pg.vert_stride; \
        out[dst_idx].x     = u2f(src[_b + 0]); \
        out[dst_idx].y     = u2f(src[_b + 1]); \
        out[dst_idx].z     = 0.0f; \
        out[dst_idx].rhw   = 1.0f; \
        out[dst_idx].u     = u2f(src[_b + 2]); \
        out[dst_idx].v     = u2f(src[_b + 3]); \
        out[dst_idx].color = src[_b + 4]; \
    } while(0)

    if (is_quads) {
        /* Convert quads (v0,v1,v2,v3) → two triangles (v0,v1,v2), (v0,v2,v3) */
        uint32_t out_idx = 0;
        for (uint32_t q = 0; q < num_quads; q++) {
            uint32_t qi = q * 4;
            CONVERT_VERT(out_idx + 0, qi + 0);  /* tri 1: v0 */
            CONVERT_VERT(out_idx + 1, qi + 1);  /* tri 1: v1 */
            CONVERT_VERT(out_idx + 2, qi + 2);  /* tri 1: v2 */
            CONVERT_VERT(out_idx + 3, qi + 0);  /* tri 2: v0 */
            CONVERT_VERT(out_idx + 4, qi + 2);  /* tri 2: v2 */
            CONVERT_VERT(out_idx + 5, qi + 3);  /* tri 2: v3 */
            out_idx += 6;
        }
    } else {
        for (uint32_t i = 0; i < num_verts; i++) {
            CONVERT_VERT(i, i);
        }
    }
    #undef CONVERT_VERT

    /* Chyron scroll: shift X for vertices in the chyron Y band (366-382).
     * Simple continuous scroll — no per-vertex wrapping to avoid artifacts
     * from split triangle-strip quads spanning the screen. */
    if (g_pg.chyron_scroll_offset != 0.0f && out_vert_count >= 6) {
        /* Check if this draw is in the chyron band */
        int is_chyron = 1;
        for (uint32_t i = 0; i < (out_vert_count < 8 ? out_vert_count : 8); i++) {
            if (out[i].y < 360.0f || out[i].y > 390.0f) {
                is_chyron = 0;
                break;
            }
        }
        if (is_chyron) {
            /* Find the total text width */
            float min_x = 9999.0f, max_x = -9999.0f;
            for (uint32_t i = 0; i < out_vert_count; i++) {
                if (out[i].x < min_x) min_x = out[i].x;
                if (out[i].x > max_x) max_x = out[i].x;
            }
            float text_width = max_x - min_x;

            /* Scroll loops: text slides left, then resets to start position.
             * Total cycle = text scrolls fully off-left + re-enters from right. */
            float cycle = text_width + 640.0f;
            float scroll = fmodf(g_pg.chyron_scroll_offset, cycle);

            /* Apply uniform shift to ALL vertices (no per-vertex wrap) */
            for (uint32_t i = 0; i < out_vert_count; i++) {
                out[i].x -= scroll;
            }
        }
    }

    /* Log first few draws' vertex positions (once) */
    if (g_pg.stats.draw_calls < 3 && num_verts >= 3) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw verts (mode=%u, %u in → %u out):\n",
                g_pg.draw_mode, num_verts, out_vert_count);
        uint32_t show = num_verts < 8 ? num_verts : 8;
        for (uint32_t i = 0; i < show; i++) {
            uint32_t b = i * g_pg.vert_stride;
            fprintf(stderr, "  [%u] pos=(%.1f, %.1f) uv=(%.3f, %.3f) color=0x%08X\n",
                    i, u2f(src[b+0]), u2f(src[b+1]), u2f(src[b+2]), u2f(src[b+3]), src[b+4]);
        }
    }

    /* Get D3D8 device */
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    if (!dev) return;

    /* Set up 2D render state — always enable alpha for menu transparency */
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    /* Set FVF for pre-transformed 2D with texture */
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    /* Bind texture based on NV2A VRAM offset.
     * Game-specific texture mapping is handled via GAME_HAS_FONT_ATLAS
     * compile flag. Generic path uses vertex color only. */
#ifdef GAME_HAS_FONT_ATLAS
    if (g_textures_loaded) {
        if (!g_pg.texture_lookup_done) {
            g_pg.texture_lookup_done = 1;
            fprintf(stderr, "[PGRAPH-D3D11] Texture lookup init (global_txd has %d textures)\n",
                    g_global_txd.count);
            for (int ti = 0; ti < g_global_txd.count; ti++) {
                fprintf(stderr, "    [%3d] %-24s %3ux%-3u fmt=0x%X\n",
                        ti, g_global_txd.entries[ti].name,
                        g_global_txd.entries[ti].width,
                        g_global_txd.entries[ti].height,
                        g_global_txd.entries[ti].format);
            }
        }

        IDirect3DTexture8 *tex = NULL;
        uint32_t vram_off = g_pg.tex[0].offset;
        switch (vram_off) {
            case 0x03C1ED00: tex = txd_find(&g_global_txd, "B3Logo"); break;
            case 0x03C24700: tex = txd_find(&g_global_txd, "bg"); break;
            case 0x03C24B80: tex = txd_find(&g_global_txd, "big_curve"); break;
            case 0x03C7BE00: tex = txd_find(&g_global_txd, "Buttons"); break;
            case 0x03C95700: tex = txd_find(&g_global_txd, "dpad"); break;
            case 0x03C95980: tex = txd_find(&g_global_txd, "FE"); break;
            case 0x03CA1A80: tex = txd_find(&g_global_txd, "small_curve"); break;
            case 0x03D57000: tex = txd_find(&g_global_txd, "box_curve"); break;
            case 0x03CB9200: tex = txd_find(&g_global_txd, "grid"); break;
            case 0x02EC0400:
                dev->lpVtbl->EndScene(dev);
                g_pg.inline_count = 0;
                return;
            case 0x021C4100:
                if (!g_pg.font_atlas) {
                    g_pg.font_atlas = create_dxt5_texture(dev,
                        FONT_ATLAS_WIDTH, FONT_ATLAS_HEIGHT,
                        font_atlas_dxt5, FONT_ATLAS_SIZE);
                }
                tex = g_pg.font_atlas;
                break;
            case 0: tex = NULL; break;
            default: tex = NULL; break;
        }

        if (tex) {
            dev->lpVtbl->SetTexture(dev, 0, (IDirect3DBaseTexture8 *)tex);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 3 /*COLORARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 4 /*MODULATE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 2 /*TEXTURE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 6 /*ALPHAARG2*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 13 /*ADDRESSU*/, 3 /*CLAMP*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 14 /*ADDRESSV*/, 3 /*CLAMP*/);
        } else {
            /* No texture — use vertex color only */
            dev->lpVtbl->SetTexture(dev, 0, NULL);
            dev->lpVtbl->SetTextureStageState(dev, 0, 1 /*COLOROP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 2 /*COLORARG1*/, 0 /*DIFFUSE*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 4 /*ALPHAOP*/, 2 /*SELECTARG1*/);
            dev->lpVtbl->SetTextureStageState(dev, 0, 5 /*ALPHAARG1*/, 0 /*DIFFUSE*/);
        }
    } else {
        dev->lpVtbl->SetTexture(dev, 0, NULL);
    }
#else
    /* Generic path: no game-specific texture lookup, use vertex color only */
    {
        dev->lpVtbl->SetTexture(dev, 0, NULL);
        dev->lpVtbl->SetTextureStageState(dev, 0, 1, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 2, 0 /*DIFFUSE*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 4, 2 /*SELECTARG1*/);
        dev->lpVtbl->SetTextureStageState(dev, 0, 5, 0 /*DIFFUSE*/);
    }
#endif

    /* Begin scene if needed */
    dev->lpVtbl->BeginScene(dev);

    /* Draw */
    /* actual_prim_type, not g_pg.d3d_prim_type: the quad path above rewrote
     * the vertices into a triangle list, and drawing those as the original
     * primitive reinterprets the same buffer as the wrong shape. */
    dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)actual_prim_type,
                                  prim_count, out, sizeof(OutputVertex));

    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += num_verts;

    if (g_pg.stats.draw_calls <= 5 || (g_pg.stats.draw_calls % 1000) == 0) {
        fprintf(stderr, "[PGRAPH-D3D11] Draw #%u: %u verts, prim=%d, prims=%u\n",
                g_pg.stats.draw_calls, num_verts, g_pg.d3d_prim_type, prim_count);
    }
}

/* ══════════════════════════════════════════════════════════════════════
 * Method Handler
 * ══════════════════════════════════════════════════════════════════════ */

#if defined(__ANDROID__)
void pgraph_d3d11_note_index_words(uint32_t words)
{
    if (g_pg.initialized) {
        g_pg.stats.methods_handled += words;
        g_pg.stats.methods_ignored += words;
    }
}
#endif

int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{
    if (!g_pg.initialized)
        return 0;

    g_pg.stats.methods_handled++;

    switch (method) {

    /* ── Draw Begin/End ── */
    case NV097_SET_BEGIN_END:
        if (param == 0) {
            /* END: submit accumulated vertices */
            if (g_pg.in_draw) {
                submit_draw();
                g_pg.in_draw = 0;
            }
        } else {
            /* BEGIN: start new draw */
            g_pg.in_draw = 1;
            g_pg.draw_mode = param;
            g_pg.d3d_prim_type = nv2a_draw_mode_to_d3d(param);
            g_pg.inline_count = 0;
        }
        return 1;

    /* ── Inline Vertex Data ── */
    case NV097_INLINE_ARRAY:
        if (g_pg.in_draw && g_pg.inline_count < MAX_INLINE_VERTS * INLINE_VERT_DWORDS) {
            g_pg.inline_data[g_pg.inline_count++] = param;
        }
        return 1;

    /* ── Clear ── */
    case NV097_SET_COLOR_CLEAR_VALUE:
        g_pg.clear_color = param;
        return 1;

    case NV097_SET_CLEAR_RECT_HORIZONTAL:
        g_pg.clear_rect_h = param;
        return 1;

    case NV097_SET_CLEAR_RECT_VERTICAL:
        g_pg.clear_rect_v = param;
        return 1;

    case NV097_CLEAR_SURFACE:
    {
        IDirect3DDevice8 *dev = xbox_GetD3DDevice();
        if (dev) {
            uint32_t flags = 0;
            if (param & 0xF0) flags |= 1;  /* D3DCLEAR_TARGET */
            if (param & 0x01) flags |= 2;  /* D3DCLEAR_ZBUFFER */
            if (param & 0x02) flags |= 4;  /* D3DCLEAR_STENCIL */
            /* Depth and stencil clear to the title's value, packed as the
             * zeta format stores it (Z24S8: depth in the top 24 bits). */
            int z16 = g_pg.zmax < 65536.0f;
            float zc = (float)(z16 ? (g_pg.zstencil_clear & 0xFFFFu)
                                   : (g_pg.zstencil_clear >> 8)) / g_pg.zmax;
            DWORD sc = z16 ? 0 : (g_pg.zstencil_clear & 0xFFu);
            HRESULT chr = dev->lpVtbl->Clear(dev, 0, NULL, flags,
                                             g_pg.clear_color, zc > 1.0f ? 1.0f : zc, sc);
            {
                /* RECOMP_CLEAR_TRACE=N: the first N clears with their depth. */
                static long trace = -1;
                static long traced;
                if (trace < 0) {
                    const char *e = getenv("RECOMP_CLEAR_TRACE");
                    trace = e ? strtol(e, NULL, 0) : 0;
                }
                if (traced < trace && (param & 0x03)) {
                    traced++;
                    fprintf(stderr, "[PGRAPH-D3D11] clear 0x%X: depth value 0x%08X -> %.6f (zmax %.0f),"
                                    " stencil %lu%c", param, g_pg.zstencil_clear, zc, g_pg.zmax,
                            (unsigned long)sc, 10);
                }
            }
            if (g_pg.stats.clears < 3)
                fprintf(stderr, "[PGRAPH-D3D11] clear #%u flags 0x%X colour "
                                "0x%08X -> 0x%08lX\n",
                        g_pg.stats.clears, flags, g_pg.clear_color,
                        (unsigned long)chr);
        }
        g_pg.stats.clears++;
        return 1;
    }

    /* ── Render State ── */
    case NV097_SET_DEPTH_TEST_ENABLE:
        g_pg.depth_test = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_ENABLE:
        g_pg.blend_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_BLEND_FUNC_SFACTOR:
        g_pg.blend_sfactor = param;
        return 1;

    case NV097_SET_BLEND_FUNC_DFACTOR:
        g_pg.blend_dfactor = param;
        return 1;

    case 0x034C:                            /* SET_BLEND_COLOR, ARGB */
        g_pg.blend_color = param;
        return 1;

    case 0x0350:                            /* SET_BLEND_EQUATION */
        g_pg.blend_equation = param;
        return 1;

    case NV097_SET_CULL_FACE_ENABLE:
        g_pg.cull_enable = param ? 1 : 0;
        return 1;

    case NV097_SET_ALPHA_TEST_ENABLE:
        g_pg.alpha_test = param ? 1 : 0;
        return 1;

    case 0x033C:                          /* NV097_SET_ALPHA_FUNC */
        g_pg.alpha_func = param;
        return 1;

    case 0x0340:                          /* NV097_SET_ALPHA_REF */
        g_pg.alpha_ref = param & 0xFF;
        return 1;

    case NV097_SET_COLOR_MASK:
        g_pg.color_mask = param;
        return 1;

    case NV097_SET_DEPTH_FUNC:
        g_pg.depth_func = param;
        return 1;

    case NV097_SET_DEPTH_MASK:
        g_pg.depth_mask = param ? 1 : 0;
        return 1;

    case NV097_SET_ZSTENCIL_CLEAR_VALUE:
        g_pg.zstencil_clear = param;
        return 1;

    /* Stencil. Def Jam draws its shadows through it: each shadow triangle
     * passes where the stencil is not yet 1 and writes 1, so overlapping
     * triangles darken a spot once. */
    case 0x032C: g_pg.stencil_enable = param ? 1 : 0; return 1;
    case 0x0360: g_pg.stencil_wmask = param & 0xFF;   return 1;
    case 0x0364: g_pg.stencil_func = param;            return 1;
    case 0x0368: g_pg.stencil_ref = param & 0xFF;     return 1;
    case 0x036C: g_pg.stencil_rmask = param & 0xFF;   return 1;
    case 0x0370: g_pg.stencil_fail = param;            return 1;
    case 0x0374: g_pg.stencil_zfail = param;           return 1;
    case 0x0378: g_pg.stencil_zpass = param;           return 1;

    case NV097_SET_SURFACE_FORMAT:
        /* Bits 7:4 are the zeta format: 1 Z16, 2 Z24S8. Depth arrives from the
         * vertex program in that format's units, 0..zmax.
         * ponytail: the float-depth mode (SET_CONTROL0) is not modelled. */
        g_pg.zmax = (((param >> 4) & 0xF) == 1) ? 65535.0f : 16777215.0f;
        return 1;

    case NV097_SET_SHADE_MODE:
        /* 1=flat, 2=gouraud — we always use gouraud */
        return 1;

    /* ── Viewport ── */
    case NV097_SET_VIEWPORT_OFFSET:
    case NV097_SET_VIEWPORT_OFFSET + 4:
    case NV097_SET_VIEWPORT_OFFSET + 8:
    case NV097_SET_VIEWPORT_OFFSET + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
        g_pg.vp_offset[idx] = u2f(param);
        return 1;
    }

    case NV097_SET_VIEWPORT_SCALE:
    case NV097_SET_VIEWPORT_SCALE + 4:
    case NV097_SET_VIEWPORT_SCALE + 8:
    case NV097_SET_VIEWPORT_SCALE + 12:
    {
        int idx = (method - NV097_SET_VIEWPORT_SCALE) / 4;
        g_pg.vp_scale[idx] = u2f(param);
        return 1;
    }

    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        g_pg.surface_clip_h = param;
        return 1;

    case NV097_SET_SURFACE_CLIP_VERTICAL:
        g_pg.surface_clip_v = param;
        return 1;

    /* ── Texture state tracking (4 stages, 0x40 stride) ── */
    case NV097_SET_TEXTURE_OFFSET:
    case NV097_SET_TEXTURE_OFFSET + 0x40:
    case NV097_SET_TEXTURE_OFFSET + 0x80:
    case NV097_SET_TEXTURE_OFFSET + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_OFFSET) / 0x40;
        g_pg.tex[stage].offset = param;
        return 1;
    }
    case NV097_SET_TEXTURE_FORMAT:
    case NV097_SET_TEXTURE_FORMAT + 0x40:
    case NV097_SET_TEXTURE_FORMAT + 0x80:
    case NV097_SET_TEXTURE_FORMAT + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_FORMAT) / 0x40;
        g_pg.tex[stage].format = param;
        return 1;
    }
    case NV097_SET_TEXTURE_CONTROL0:
    case NV097_SET_TEXTURE_CONTROL0 + 0x40:
    case NV097_SET_TEXTURE_CONTROL0 + 0x80:
    case NV097_SET_TEXTURE_CONTROL0 + 0xC0:
    {
        int stage = (method - NV097_SET_TEXTURE_CONTROL0) / 0x40;
        g_pg.tex[stage].control0 = param;
        g_pg.tex[stage].enabled = (param >> 30) & 1;
        return 1;
    }

    default:
        /* Check if it's in a known range we can safely ignore */
        if ((method >= 0x0B80 && method < 0x0C00) ||  /* Transform program */
            (method >= 0x0E00 && method < 0x1000) ||  /* Transform constants */
            (method >= 0x1680 && method < 0x1780) ||  /* Vertex array format/offset */
            (method >= 0x1B00 && method < 0x1C00) ||  /* Texture registers */
            (method >= 0x1D60 && method < 0x1EA0) ||  /* Combiners */
            method == 0x0100 ||                        /* NOP */
            method == 0x0180 ||                        /* SET_OBJECT */
            method == 0x0394 ||                        /* TRANSFORM_EXECUTION_MODE */
            method == 0x0398 ||                        /* TRANSFORM_PROGRAM_CXT_WRITE_EN */
            method == 0x039C ||                        /* TRANSFORM_PROGRAM_LOAD */
            method == 0x01E0 ||                        /* SHADER_STAGE_PROGRAM */
            method == NV097_SET_FLIP_READ ||           /* 0x0120 */
            method == NV097_SET_FLIP_WRITE ||          /* 0x0124 */
            method == NV097_SET_FLIP_MODULO ||         /* 0x0128 */
            method == NV097_FLIP_INCREMENT_WRITE ||    /* 0x012C */
            method == NV097_FLIP_STALL)                /* 0x0130 */
        {
            return 1;  /* Silently handled (ignored but acknowledged) */
        }

        g_pg.stats.methods_ignored++;
        return 0;  /* Truly unhandled */
    }
}

/* Textures the title has programmed, uploaded once and kept.
 *
 * The binding above this is a captured font atlas for one title, chosen by a
 * hardcoded address. This is the general form: the decoder resolves where the
 * texture lives and what it is, and the data goes to the GPU as it stands.
 * Compressed formats are the common case for a title's own art and copy
 * straight across, because the block layout is the one Direct3D wants. */
/* Least recently used goes first when it is full. It used to stop at 64 and
 * bind nothing for every texture after that -- so from Def Jam's main menu on,
 * when the loading screen, intro, title and header had used up the slots,
 * every new texture drew untextured, in its white vertex colour. */
static int g_bind_stage;      /* the stage texture binds go to (0..3) */
static int g_pack_bound[4];

int pgraph_d3d11_try_texture_pack(const RecompTextureSource *source,
                                RecompTextureSample sample, void *user)
{
    IDirect3DDevice8 *dev = xbox_GetD3DDevice();
    int bound = g_pg.initialized && dev && texture_pack_bind(dev, g_bind_stage, source, sample, user);
    g_pack_bound[g_bind_stage] = bound;
    if (bound) {
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 1, 4);
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 2, 2);
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 3, 0);
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 4, 4);
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 5, 2);
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 6, 0);
    }
    return bound;
}
#define READY_TEX_CACHE 256
static struct {
    uint32_t addr, format, width, height;
    IDirect3DTexture8 *tex;
    /* What the bytes looked like at the last upload, and when that was last
     * checked. See ready_tex_sig. */
    uint64_t sig;
    ULONGLONG checked_ms;
    ULONGLONG used_ms;                  /* last bound, for eviction */
    uint32_t bytes;                     /* the level's size, for the cache's statistics */
} s_ready_tex[READY_TEX_CACHE];
static int s_ready_tex_count;

/* Bytes a compressed level occupies, or 0 if the format is not one. */
static uint32_t compressed_size(uint32_t fmt, uint32_t w, uint32_t h)
{
    uint32_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    switch (fmt) {
    case 0x0C: return blocks * 8;    /* DXT1 */
    case 0x0E:                       /* DXT3 */
    case 0x0F: return blocks * 16;   /* DXT5 */
    default:   return 0;
    }
}

/* A texture the title rewrites in place has to be uploaded again.
 *
 * The cache is keyed by address, so a surface the CPU refills every frame --
 * a movie decoded into the same 640x480 buffer, which is how Def Jam's intro
 * movies reach the screen -- was uploaded once, blank, and shown blank for
 * the rest of the movie. Each cached texture is re-read at most once every
 * few milliseconds and uploaded again when its signature changes.
 *
 * ponytail: the signature samples every 61st word (plus the last), which is
 * enough for a decoded video frame and cheap enough to run on every cached
 * texture each frame; a title that patches a few texels of a large atlas in
 * place can slip through it. A full hash, or watching the guest pages for
 * writes, is the upgrade if that ever shows. */
static uint64_t ready_tex_sig(const void *data, uint32_t bytes)
{
    const uint32_t *w = (const uint32_t *)data;
    uint32_t n = bytes / 4, i;
    uint64_t h = 1469598103934665603ull;
    for (i = 0; i < n; i += 61) {
        h ^= w[i];
        h *= 1099511628211ull;
    }
    if (n) {
        h ^= w[n - 1];
        h *= 1099511628211ull;
    }
    return h;
}

UINT d3d8_format_bpp(D3DFORMAT fmt);   /* d3d8_resources.c */

/* Bytes of guest data behind a texture and its shape, or 0 when the format
 * has no known size. */
static uint32_t ready_tex_bytes(uint32_t fmt, uint32_t w, uint32_t h,
                                int *compressed, uint32_t *bpp)
{
    uint32_t bytes = compressed_size(fmt, w, h);
    *compressed = bytes != 0;
    *bpp = 0;
    if (bytes)
        return bytes;
    *bpp = d3d8_format_bpp((D3DFORMAT)fmt) / 8u;
    return *bpp ? w * h * *bpp : 0;
}

/* Copy the title's bytes into the host texture. Three shapes of texture, and
 * the D3D8 layer already knows which is which, so ask it rather than keeping a
 * second table here:
 *
 *   compressed  - DXT blocks, copied straight through;
 *   swizzled    - Morton order, which xbox_unswizzle_rect undoes;
 *   linear      - rows, which still have to be copied one at a time
 *                 because the locked pitch is the host's, not ours.
 *
 * Only the compressed ones used to upload. Everything else bound nothing, so
 * a menu drawing its background from a swizzled surface and its text from a
 * DXT atlas showed half of itself. Returns 0 if the texture was not filled. */
static int ready_tex_fill(IDirect3DTexture8 *tex, const void *data,
                          uint32_t width, uint32_t height, uint32_t nv2a_format)
{
    D3DLOCKED_RECT lr;
    int compressed;
    uint32_t bpp;
    uint32_t bytes = ready_tex_bytes(nv2a_format, width, height, &compressed, &bpp);
    uint32_t row = width * bpp;
    const uint8_t *rows = (const uint8_t *)data;

    memset(&lr, 0, sizeof(lr));
    if (FAILED(tex->lpVtbl->LockRect(tex, 0, &lr, NULL, 0)) || !lr.pBits)
        return 0;
    /* A swizzled format goes in as it is: the D3D8 layer unswizzles what a
     * title writes into a locked swizzled texture when it uploads it
     * (d3d8_resources.c). Unswizzling here as well did it twice, and every
     * 32-bit swizzled texture -- Def Jam's particle sheets -- came out as a
     * dotted checkerboard. The paletted path never met this: it expands to a
     * linear format first. */
    if (compressed) {
        memcpy(lr.pBits, data, bytes);
    } else {
        uint32_t y;
        for (y = 0; y < height; y++)
            memcpy((uint8_t *)lr.pBits + (size_t)y * lr.Pitch,
                   rows + (size_t)y * row, row);
    }
    tex->lpVtbl->UnlockRect(tex, 0);
    return 1;
}

static int s_ready_tex_force;           /* next hit refills now (replace) */

int pgraph_d3d11_replace_texture_ready(uint32_t guest_addr, const void *data,
                                       uint32_t width, uint32_t height,
                                       uint32_t nv2a_format)
{
    int bound;
    s_ready_tex_force = data != NULL;
    bound = pgraph_d3d11_set_texture_ready(guest_addr, data, width, height, nv2a_format);
    s_ready_tex_force = 0;
    return bound;
}

int pgraph_d3d11_set_texture_ready(uint32_t guest_addr, const void *data,
                                   uint32_t width, uint32_t height,
                                   uint32_t nv2a_format)
{
    IDirect3DDevice8 *dev;
    IDirect3DTexture8 *tex = NULL;
    uint32_t bytes, bpp;
    int compressed;
    int i;

    g_pack_bound[g_bind_stage] = 0;

    if (!g_pg.initialized)
        return 0;
    dev = xbox_GetD3DDevice();
    if (!dev)
        return 0;

    if (!width || !height) {
        dev->lpVtbl->SetTexture(dev, g_bind_stage, NULL);
        return 0;
    }

    for (i = 0; i < s_ready_tex_count; i++) {
        if (s_ready_tex[i].addr == guest_addr
                && s_ready_tex[i].format == nv2a_format
                && s_ready_tex[i].width == width
                && s_ready_tex[i].height == height) {
            tex = s_ready_tex[i].tex;
            s_ready_tex[i].used_ms = GetTickCount64();
            break;
        }
    }

    if (tex) {
        /* No data means the caller converted this texture itself and has
         * nothing new to say about it (a palettised texture, keyed by its
         * palette): bind what was uploaded without reading anything. */
        ULONGLONG now = GetTickCount64();
        if (data && (s_ready_tex_force || now - s_ready_tex[i].checked_ms >= 8)) {
            uint64_t sig;
            s_ready_tex[i].checked_ms = now;
            bytes = ready_tex_bytes(nv2a_format, width, height, &compressed, &bpp);
            sig = ready_tex_sig(data, bytes);
            if ((s_ready_tex_force || sig != s_ready_tex[i].sig)
                    && ready_tex_fill(tex, data, width, height, nv2a_format)) {
                static uint32_t refreshed;
                s_ready_tex[i].sig = sig;
                g_pg.stats.textures_uploaded++;
                g_pg.stats.texture_upload_bytes += bytes;
                if (refreshed++ < 4 || refreshed % 1000 == 0)
                    fprintf(stderr, "[PGRAPH-D3D11] texture 0x%08X %ux%u fmt 0x%02X"
                                    " changed in place; uploaded again (#%u)\n",
                            guest_addr, width, height, nv2a_format, refreshed);
            }
        }
    } else {
        if (!data) {
            dev->lpVtbl->SetTexture(dev, g_bind_stage, NULL);
            return 0;
        }
        bytes = ready_tex_bytes(nv2a_format, width, height, &compressed, &bpp);
        if (!bytes) {
            static uint32_t moaned;
            if (moaned++ < 8)
                fprintf(stderr, "[PGRAPH-D3D11] texture format 0x%02X has"
                                " no known size (%ux%u)\n",
                        nv2a_format, width, height);
            dev->lpVtbl->SetTexture(dev, g_bind_stage, NULL);
            return 0;
        }
        int slot = s_ready_tex_count;
        if (slot >= READY_TEX_CACHE) {
            int j;
            slot = 0;
            for (j = 1; j < READY_TEX_CACHE; j++)
                if (s_ready_tex[j].used_ms < s_ready_tex[slot].used_ms)
                    slot = j;
            /* Off every stage that holds it, not only the one being bound:
             * the device keeps a bare pointer per stage, and the texture
             * being evicted may sit in another (stages 1-3 are in use since
             * the combiners, and a stage can stay bound across many draws).
             * Freed while still bound there, the next draw read its format
             * out of released memory. */
            if (s_ready_tex[slot].tex) {
                DWORD st;
                for (st = 0; st < 4; st++) {
                    IDirect3DBaseTexture8 *cur = NULL;
                    if (SUCCEEDED(dev->lpVtbl->GetTexture(dev, st, &cur)) && cur) {
                        cur->lpVtbl->Release(cur);          /* GetTexture's reference */
                        if (cur == (IDirect3DBaseTexture8 *)s_ready_tex[slot].tex)
                            dev->lpVtbl->SetTexture(dev, st, NULL);
                    }
                }
            }
            if (s_ready_tex[slot].tex) {
                s_ready_tex[slot].tex->lpVtbl->Release(s_ready_tex[slot].tex);
                g_pg.stats.texture_evictions++;
                g_pg.stats.textures_cached--;
                g_pg.stats.texture_cache_bytes -= s_ready_tex[slot].bytes;
            }
            s_ready_tex[slot].tex = NULL;
            {
                static unsigned evicted;
                if (evicted++ < 4 || evicted % 1000 == 0)
                    fprintf(stderr, "[PGRAPH-D3D11] texture cache full; evicted 0x%08X (#%u)\n",
                            s_ready_tex[slot].addr, evicted);
            }
        }
        if (FAILED(dev->lpVtbl->CreateTexture(dev, width, height, 1, 0,
                                              nv2a_format, 0, &tex)) || !tex) {
            dev->lpVtbl->SetTexture(dev, g_bind_stage, NULL);
            return 0;
        }
        if (!ready_tex_fill(tex, data, width, height, nv2a_format)) {
            tex->lpVtbl->Release(tex);
            dev->lpVtbl->SetTexture(dev, g_bind_stage, NULL);
            return 0;
        }
        s_ready_tex[slot].addr   = guest_addr;
        s_ready_tex[slot].format = nv2a_format;
        s_ready_tex[slot].width  = width;
        s_ready_tex[slot].height = height;
        s_ready_tex[slot].tex    = tex;
        s_ready_tex[slot].sig    = ready_tex_sig(data, bytes);
        s_ready_tex[slot].checked_ms = GetTickCount64();
        s_ready_tex[slot].used_ms = s_ready_tex[slot].checked_ms;
        s_ready_tex[slot].bytes  = bytes;
        g_pg.stats.textures_uploaded++;
        g_pg.stats.texture_upload_bytes += bytes;
        g_pg.stats.textures_cached++;
        g_pg.stats.texture_cache_bytes += bytes;
        if (slot == s_ready_tex_count)
            s_ready_tex_count++;
        /* Which of the three shapes it took, because "uploaded" on its own
         * does not say whether the swizzled path or the linear one ran, and
         * those are the two that are new. Capped, since a title binds the same
         * atlas thousands of times. */
        {
            static uint32_t reported;
            if (reported++ < 12)
                fprintf(stderr, "[PGRAPH-D3D11] uploaded %s texture 0x%08X "
                                "%ux%u fmt 0x%02X (%u bytes)\n",
                        compressed ? "compressed"
                            : d3d8_format_is_swizzled(nv2a_format) ? "swizzled"
                            : "linear",
                        guest_addr, width, height, nv2a_format, bytes);
            fflush(stderr);
        }
    }

    dev->lpVtbl->SetTexture(dev, g_bind_stage, (IDirect3DBaseTexture8 *)tex);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 1  /*COLOROP*/,   4 /*MODULATE*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 2  /*COLORARG1*/, 2 /*TEXTURE*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 3  /*COLORARG2*/, 0 /*DIFFUSE*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 4  /*ALPHAOP*/,   4 /*MODULATE*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 5  /*ALPHAARG1*/, 2 /*TEXTURE*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 6  /*ALPHAARG2*/, 0 /*DIFFUSE*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 13 /*ADDRESSU*/,  3 /*CLAMP*/);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 14 /*ADDRESSV*/,  3 /*CLAMP*/);
    return 1;
}

/* Wrap, mirror and border as the title asked. Every bind used to leave both
 * axes clamped, so a sprite whose coordinates run past 1 to pick a second
 * copy from its atlas sampled the atlas's edge column instead: Def Jam's back
 * rows of crowd came out as flat brown silhouettes. */
void pgraph_d3d11_set_texture_address(uint32_t nv2a_u, uint32_t nv2a_v)
{
    static const DWORD map[8] = {
        3 /*0: unset -> clamp*/, 1 /*WRAP*/, 2 /*MIRROR*/, 3 /*CLAMP*/,
        4 /*BORDER*/, 3 /*CLAMP_OGL*/, 3, 3
    };
    IDirect3DDevice8 *dev;
    if (!g_pg.initialized || !(dev = xbox_GetD3DDevice()))
        return;
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 13 /*ADDRESSU*/, map[nv2a_u & 7]);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 14 /*ADDRESSV*/, map[nv2a_v & 7]);
}

/* Nearest or linear, as SET_TEXTURE_FILTER (0x1B14) asks: minification in
 * bits 16-23 (1, 3, 5 sample one texel; 2, 4, 6, 7 filter), magnification in
 * bits 24-27 (1 one texel, 2 and 4 filter). Everything used to be drawn
 * linear, and a sprite the title wants unfiltered -- a cut-out whose
 * transparent texels hold a key colour -- picked that colour up along its
 * edge: green rims on the creator's photographs. Mip levels are not uploaded,
 * so the mip half of the setting is left out. 0 (never set) keeps linear. */
void pgraph_d3d11_set_texture_filter(uint32_t nv2a_filter)
{
    IDirect3DDevice8 *dev;
    uint32_t min = (nv2a_filter >> 16) & 0xFF, mag = (nv2a_filter >> 24) & 0xF;
    DWORD dmin = (min == 1 || min == 3 || min == 5) ? 1 /*POINT*/ : 2 /*LINEAR*/;
    DWORD dmag = (mag == 1) ? 1 : 2;
    if (!g_pg.initialized || !(dev = xbox_GetD3DDevice()))
        return;
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 16 /*MAGFILTER*/, dmag);
    dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 17 /*MINFILTER*/, dmin);
    if (texture_pack_active()) {
        DWORD mip = g_pack_bound[g_bind_stage] ? ((min == 3 || min == 4) ? 1 : (min >= 5 && min <= 7) ? 2 : 0) : 0;
        dev->lpVtbl->SetTextureStageState(dev, g_bind_stage, 18 /*MIPFILTER*/, mip);
    }
}

/* Which texture stage the next set_texture_ready / set_texture_address
 * binds (0..3); and a stage emptied. */
void pgraph_d3d11_bind_stage(int stage)
{
    g_bind_stage = (stage >= 0 && stage < 4) ? stage : 0;
}

/* The surface clip (NV097 0x0200/0x0204) the next draws are confined to. */
void pgraph_d3d11_set_surface_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    extern void d3d8_SetScissorRect(UINT x, UINT y, UINT w, UINT h);
    if (g_pg.initialized)
        d3d8_SetScissorRect(x, y, w, h);
}

void pgraph_d3d11_unbind_stage(int stage)
{
    IDirect3DDevice8 *dev;
    if (!g_pg.initialized || !(dev = xbox_GetD3DDevice()) || stage < 0 || stage > 3)
        return;
    dev->lpVtbl->SetTexture(dev, (DWORD)stage, NULL);
    g_pack_bound[stage] = 0;
}

/* The title's register combiners for the next draws (M4f), from the NV097
 * words as the executor recorded them: a = 0x260..0x28C (alpha inputs, final
 * combiner words at [10] and [11]), b = 0xA60..0xAFC (factors 0 and 1, alpha
 * outputs, colour inputs), c = 0x1E40..0x1E7C (colour outputs, control at
 * [8], texture shader at [12]), and the final combiner's two constants.
 * RECOMP_NO_COMBINERS=1 keeps the old texture-times-diffuse. */
extern void d3d8_combiners_set_nv2a(const DWORD color_icw[8], const DWORD alpha_icw[8],
                                    const DWORD color_ocw[8], const DWORD alpha_ocw[8],
                                    const DWORD factor0[8], const DWORD factor1[8],
                                    DWORD control, DWORD final_cw0, DWORD final_cw1,
                                    DWORD final_c0, DWORD final_c1, DWORD shader_stages);
extern void d3d8_combiners_clear_nv2a(void);

void pgraph_d3d11_set_combiners(const uint32_t *a, const uint32_t *b, const uint32_t *c,
                                uint32_t final_c0, uint32_t final_c1)
{
    static int off = -1;
    if (off < 0)
        off = getenv("RECOMP_NO_COMBINERS") != NULL;
    if (off || !a || !b || !c) {
        d3d8_combiners_clear_nv2a();
        return;
    }
    d3d8_combiners_set_nv2a((const DWORD *)&b[24], (const DWORD *)&a[0],
                            (const DWORD *)&c[0], (const DWORD *)&b[16],
                            (const DWORD *)&b[0], (const DWORD *)&b[8],
                            c[8], a[10], a[11], final_c0, final_c1, c[12]);
}

static void (*s_frame_end_callback)(void);
void pgraph_d3d11_set_frame_end_callback(void (*callback)(void))
{
    s_frame_end_callback = callback;
}
extern uint64_t g_pf_gather_ns, g_pf_draw_ns, g_pf_batches;
extern uint64_t defjam_pf_now(void);
void pgraph_d3d11_frame_end(void)
{
    /* DEFJAM_V5 */
    {
        static unsigned pf_n;
        static uint64_t pf_prev;
        uint64_t now = defjam_pf_now();
        if (!pf_prev) pf_prev = now;
        if (++pf_n >= 240) {
            double fr = (double)pf_n;
            fprintf(stderr, "[PERF] %u frames: %.2f ms/frame wall, %.2f ms gather+pack, "
                            "%.2f ms draw-submit, %.0f batches/frame%c",
                    pf_n, (double)(now - pf_prev) / 1e6 / fr, (double)g_pf_gather_ns / 1e6 / fr,
                    (double)g_pf_draw_ns / 1e6 / fr, (double)g_pf_batches / fr, 10);
            pf_n = 0; pf_prev = now;
            g_pf_gather_ns = g_pf_draw_ns = g_pf_batches = 0;
        }
    }
    pgraph_d3d11_flush();
    if (s_frame_end_callback)
        s_frame_end_callback();
}
void pgraph_d3d11_set_combiner_state(const Nv2aCombiner *rc)
{
    static int off = -1;
    if (off < 0) off = getenv("RECOMP_NO_COMBINERS") != NULL;
    if (off || !rc) {
        d3d8_combiners_clear_nv2a();
        return;
    }
    d3d8_combiners_set_nv2a((const DWORD *)rc->color_icw,
        (const DWORD *)rc->alpha_icw, (const DWORD *)rc->color_ocw,
        (const DWORD *)rc->alpha_ocw, (const DWORD *)rc->factor0,
        (const DWORD *)rc->factor1, rc->control, rc->final0, rc->final1,
        rc->final_c0, rc->final_c1, rc->stage_program);
}

/* Draw vertices the command decoder already fetched. See the header.
 *
 * Deliberately does not touch the inline accumulator or in_draw: a title can
 * use both paths, and a decoder that resolves arrays should not have to know
 * what the inline path is doing. */
/* Near-plane clipping, which the GPU does in clip space and a screen-space
 * vertex cannot. A triangle with a vertex behind the camera (w <= 0) came out
 * of the divide flipped and enormous -- x to 644,661 in a fight -- and drew
 * over the screen with depth interpolated from nonsense, so everything after
 * it failed the depth test: the whole round black under the HUD. (x*w, y*w,
 * z*w, w) is a linear function of the clip-space position, so a triangle is
 * clipped against w = CLIP_W there, its attributes interpolated with it, and
 * divided back. Returns the number of vertices written to out (0, 3 or 6). */
#define CLIP_W 1e-5f
static uint32_t clip_triangle_w(const PgraphReadyVertex *a, const PgraphReadyVertex *b,
                                const PgraphReadyVertex *c, PgraphReadyVertex *out)
{
    const PgraphReadyVertex *in[3] = { a, b, c };
    PgraphReadyVertex poly[4];
    int k, n = 0;

    for (k = 0; k < 3; k++) {
        const PgraphReadyVertex *p = in[k], *q = in[(k + 1) % 3];
        int pin = p->w > CLIP_W, qin = q->w > CLIP_W;
        if (pin) {
            if (n < 4) poly[n++] = *p;
        }
        if (pin != qin && n < 4) {
            float t = (CLIP_W - p->w) / (q->w - p->w);
            float hp[3] = { p->x * p->w, p->y * p->w, p->z * p->w };
            float hq[3] = { q->x * q->w, q->y * q->w, q->z * q->w };
            float w = CLIP_W;
            PgraphReadyVertex r;
            int ch;
            uint32_t col = 0;
            r.x = (hp[0] + t * (hq[0] - hp[0])) / w;
            r.y = (hp[1] + t * (hq[1] - hp[1])) / w;
            r.z = (hp[2] + t * (hq[2] - hp[2])) / w;
            r.w = w;
            r.u = p->u + t * (q->u - p->u);
            r.v = p->v + t * (q->v - p->v);
            for (ch = 0; ch < 4; ch++) {
                float cp = (float)((p->color >> (ch * 8)) & 0xFF);
                float cq = (float)((q->color >> (ch * 8)) & 0xFF);
                float cv = cp + t * (cq - cp);
                col |= (uint32_t)(cv < 0 ? 0 : cv > 255 ? 255 : cv + 0.5f) << (ch * 8);
            }
            r.color = col;
            poly[n++] = r;
        }
    }
    if (n < 3)
        return 0;
    out[0] = poly[0]; out[1] = poly[1]; out[2] = poly[2];
    if (n == 3)
        return 3;
    out[3] = poly[0]; out[4] = poly[2]; out[5] = poly[3];
    return 6;
}

/* A batch with any vertex at or behind the camera, as a clipped triangle
 * list; NULL if none is (the batch goes as it came). Triangles, strips, fans,
 * quads, quad strips and polygons; points and lines are left alone. */
static PgraphReadyVertex *clip_batch_w(uint32_t mode, const PgraphReadyVertex *v,
                                       uint32_t count, uint32_t *out_count)
{
    uint32_t i, tris = 0, o = 0;
    PgraphReadyVertex *out;
    int any = 0;

    for (i = 0; i < count; i++)
        if (!(v[i].w > CLIP_W)) { any = 1; break; }
    if (!any || mode < 5 || mode > 10 || count < 3)
        return NULL;
    switch (mode) {
    case 5:  tris = count / 3; break;                          /* triangles */
    case 6: case 7: case 10: tris = count - 2; break;          /* strip, fan, polygon */
    case 8:  tris = (count / 4) * 2; break;                    /* quads */
    case 9:  tris = count >= 4 ? ((count - 2) / 2) * 2 : 0; break;   /* quad strip */
    }
    {
        /* One draw at a time, on the executor's thread: a buffer that grows. */
        static PgraphReadyVertex *buf;
        static uint32_t cap;
        if (tris * 6 > cap) {
            PgraphReadyVertex *nb = (PgraphReadyVertex *)realloc(buf, (size_t)tris * 6 * sizeof *buf);
            if (!nb)
                return NULL;
            buf = nb;
            cap = tris * 6;
        }
        out = buf;
    }
    for (i = 0; i < tris; i++) {
        const PgraphReadyVertex *a, *b, *c;
        switch (mode) {
        case 5:  a = &v[i * 3]; b = &v[i * 3 + 1]; c = &v[i * 3 + 2]; break;
        case 6:  a = &v[i]; b = &v[i + 1 + (i & 1)]; c = &v[i + 2 - (i & 1)]; break;
        case 7: case 10: a = &v[0]; b = &v[i + 1]; c = &v[i + 2]; break;
        case 8: {
            uint32_t q = (i / 2) * 4;
            if (i & 1) { a = &v[q]; b = &v[q + 2]; c = &v[q + 3]; }
            else       { a = &v[q]; b = &v[q + 1]; c = &v[q + 2]; }
            break;
        }
        default: {                                             /* quad strip */
            uint32_t q = (i / 2) * 2;
            if (i & 1) { a = &v[q + 1]; b = &v[q + 3]; c = &v[q + 2]; }
            else       { a = &v[q]; b = &v[q + 1]; c = &v[q + 2]; }
            break;
        }
        }
        o += clip_triangle_w(a, b, c, out + o);
    }
    *out_count = o;
    return out;
}

/* The render state both vertex paths draw with: the title's depth, blend,
 * alpha test and colour mask, no culling, the pre-transformed vertex format
 * (which also picks the pixel shader). Returns 0 when the batch writes
 * nothing and is skipped. */
static int ready_state(IDirect3DDevice8 *dev, float *zscale)
{
    int use_depth;

    /* The same 2D state submit_draw() sets, because this path replaced it
     * rather than joining it. Without it the draw goes through whatever the
     * device was last left in, and in particular with no vertex format set at
     * all, so every vertex is rejected and the frame stays the clear colour. */
    /* The title's depth test. Def Jam's front end (EA's APT) clips with it the
     * way Flash clips with a mask: a full-screen pass with colour writes off
     * resets depth to near, the mask shape (the header's title text) is drawn
     * with colour writes off to write a farther depth where its glyphs pass
     * the alpha test, and the masked layer (the torn-paper texture) is drawn
     * with LEQUAL at that depth, so it lands only inside the glyphs. With depth
     * ignored the paper covered the whole header and the title never showed.
     * RECOMP_NO_DEPTH=1 restores the old always-off state for comparison. */
    {
        static int no_depth = -1;
        if (no_depth < 0)
            no_depth = getenv("RECOMP_NO_DEPTH") != NULL;
        use_depth = !no_depth && g_pg.depth_test;
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_ZENABLE, use_depth);
    if (use_depth) {
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZFUNC, nv2a_cmp_to_d3d(g_pg.depth_func));
        dev->lpVtbl->SetRenderState(dev, D3DRS_ZWRITEENABLE, g_pg.depth_mask);
    }
    {
        /* The title's stencil test (RECOMP_NO_STENCIL=1 leaves it off). */
        static int no_stencil = -1;
        int on;
        if (no_stencil < 0)
            no_stencil = getenv("RECOMP_NO_STENCIL") != NULL;
        on = !no_stencil && g_pg.stencil_enable;
        dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILENABLE, on);
        if (on) {
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFUNC, nv2a_cmp_to_d3d(g_pg.stencil_func));
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILREF, g_pg.stencil_ref);
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILMASK, g_pg.stencil_rmask);
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILWRITEMASK, g_pg.stencil_wmask);
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILFAIL, nv2a_stencil_op_to_d3d(g_pg.stencil_fail));
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILZFAIL, nv2a_stencil_op_to_d3d(g_pg.stencil_zfail));
            dev->lpVtbl->SetRenderState(dev, D3DRS_STENCILPASS, nv2a_stencil_op_to_d3d(g_pg.stencil_zpass));
        }
    }
    dev->lpVtbl->SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    dev->lpVtbl->SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    /* The title's own blend and alpha-test state, rather than "always blend
     * over": a pass the title draws opaque stays opaque, and cut-out text and
     * sprites keep their edges. RECOMP_FORCE_ALPHA_BLEND=1 brings back the old
     * fixed state for comparison. */
    {
        static int force = -1;
        if (force < 0)
            force = getenv("RECOMP_FORCE_ALPHA_BLEND") != NULL;
        if (force) {
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        } else {
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHABLENDENABLE, g_pg.blend_enable);
            dev->lpVtbl->SetRenderState(dev, D3DRS_SRCBLEND, nv2a_blend_to_d3d(g_pg.blend_sfactor));
            dev->lpVtbl->SetRenderState(dev, D3DRS_DESTBLEND, nv2a_blend_to_d3d(g_pg.blend_dfactor));
            dev->lpVtbl->SetRenderState(dev, D3DRS_BLENDOP, nv2a_blend_eq_to_d3d(g_pg.blend_equation));
            {
                extern void d3d8_SetBlendColor(DWORD argb);
                d3d8_SetBlendColor(g_pg.blend_color);
            }
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHATESTENABLE, g_pg.alpha_test);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAREF, g_pg.alpha_ref);
            dev->lpVtbl->SetRenderState(dev, D3DRS_ALPHAFUNC, nv2a_cmp_to_d3d(g_pg.alpha_func));
        }
    }
    dev->lpVtbl->SetVertexShader(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    /* The title's colour write mask (SET_COLOR_MASK: A 0x01000000, R 0x010000,
     * G 0x0100, B 0x01). A pass that writes only alpha or stencil must leave
     * the picture alone: Def Jam's title screen draws a full-screen quad in
     * opaque magenta with colour writes off, and with the mask ignored the
     * whole screen came out magenta. No colour channel at all: skip the draw,
     * unless it writes depth -- those are the mask passes above. */
    {
        uint32_t m = g_pg.color_mask;
        DWORD cw = ((m & 0x00010000u) ? 1u : 0u) | ((m & 0x00000100u) ? 2u : 0u)
                 | ((m & 0x00000001u) ? 4u : 0u) | ((m & 0x01000000u) ? 8u : 0u);
        if (!(cw & 7u) && !(use_depth && g_pg.depth_mask))
            return 0;
        dev->lpVtbl->SetRenderState(dev, D3DRS_COLORWRITEENABLE, (cw & 7u) ? cw : 0u);
    }
    *zscale = g_pg.zmax > 0.0f ? 1.0f / g_pg.zmax : 0.0f;
    return 1;
}

/* The converted batch. Not the stack: a batch can be 32,768 vertices, and a
 * quad list half as long again, which is over a megabyte. Only the executor
 * thread draws, so one buffer that grows is enough. */
static OutputVertex *ready_out(uint32_t count)
{
    static OutputVertex *buf;
    static uint32_t cap;
    if (count > cap) {
        OutputVertex *n = (OutputVertex *)realloc(buf, (size_t)count * sizeof(OutputVertex));
        if (!n)
            return NULL;
        buf = n;
        cap = count;
    }
    return buf;
}

void pgraph_d3d11_draw_ready(uint32_t nv2a_prim_mode,
                             const PgraphReadyVertex *verts, uint32_t count)
{
    IDirect3DDevice8 *dev;
    OutputVertex *out;
    uint32_t out_count, prim_count, i;
    int prim_type;
    float zscale;

    if (!g_pg.initialized || !verts || count < 3)
        return;
    dev = xbox_GetD3DDevice();
    if (!dev)
        return;
    {
        /* Any vertex at or behind the camera: clip the batch in clip space
         * first (clip_batch_w). RECOMP_NO_W_CLIP=1 turns it off. */
        static int no_clip = -1;
        uint32_t clipped_n = 0;
        const PgraphReadyVertex *clipped;
        if (no_clip < 0)
            no_clip = getenv("RECOMP_NO_W_CLIP") != NULL;
        clipped = no_clip ? NULL : clip_batch_w(nv2a_prim_mode, verts, count, &clipped_n);
        if (clipped) {
            if (clipped_n < 3)
                return;
            verts = clipped;
            count = clipped_n;
            nv2a_prim_mode = 5;             /* now a triangle list */
        }
    }

    if (!ready_state(dev, &zscale))
        return;

    /* Quads and quad strips have no Direct3D equivalent, so they become a
     * triangle list here. Everything else maps directly. */
    if (nv2a_prim_mode == 8 || nv2a_prim_mode == 9) {
        uint32_t quads = (nv2a_prim_mode == 8) ? count / 4
                                               : (count >= 4 ? (count - 2) / 2 : 0);
        if (!quads)
            return;
        out_count = quads * 6;
        prim_type = D3DPT_TRIANGLELIST;
        out = ready_out(out_count);
        if (!out)
            return;
        for (i = 0; i < quads; i++) {
            /* A quad strip advances two vertices at a time and the third and
             * fourth arrive swapped relative to a quad list. */
            uint32_t b = (nv2a_prim_mode == 8) ? i * 4 : i * 2;
            const PgraphReadyVertex *q[4];
            q[0] = &verts[b + 0];
            q[1] = &verts[b + 1];
            q[2] = (nv2a_prim_mode == 8) ? &verts[b + 2] : &verts[b + 3];
            q[3] = (nv2a_prim_mode == 8) ? &verts[b + 3] : &verts[b + 2];
            {
                static const int tri[6] = { 0, 1, 2, 0, 2, 3 };
                uint32_t k;
                for (k = 0; k < 6; k++) {
                    OutputVertex *o = &out[i * 6 + k];
                    const PgraphReadyVertex *v = q[tri[k]];
                    o->x = v->x; o->y = v->y; o->rhw = 1.0f;
                    o->z = v->z * zscale;
                    o->z = o->z < 0.0f ? 0.0f : o->z > 1.0f ? 1.0f : o->z;
                    o->color = v->color; o->u = v->u; o->v = v->v;
                }
            }
        }
    } else {
        out_count = count;
        prim_type = nv2a_draw_mode_to_d3d(nv2a_prim_mode);
        out = ready_out(out_count);
        if (!out)
            return;
        for (i = 0; i < count; i++) {
            out[i].x = verts[i].x; out[i].y = verts[i].y;
            out[i].z = verts[i].z * zscale;
            out[i].z = out[i].z < 0.0f ? 0.0f : out[i].z > 1.0f ? 1.0f : out[i].z;
            out[i].rhw = 1.0f;
            out[i].color = verts[i].color;
            out[i].u = verts[i].u; out[i].v = verts[i].v;
        }
    }

    switch (prim_type) {
    case D3DPT_TRIANGLELIST:  prim_count = out_count / 3; break;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN:   prim_count = out_count - 2; break;
    case D3DPT_LINELIST:      prim_count = out_count / 2; break;
    case D3DPT_LINESTRIP:     prim_count = out_count - 1; break;
    default:                  prim_count = out_count / 3; break;
    }
    if (!prim_count)
        return;

    dev->lpVtbl->BeginScene(dev);
    {
        HRESULT hr = dev->lpVtbl->DrawPrimitiveUP(dev, (D3DPRIMITIVETYPE)prim_type,
                                                  prim_count, out, sizeof(OutputVertex));
        if (FAILED(hr) && g_pg.stats.draw_calls < 5)
            fprintf(stderr, "[PGRAPH-D3D11] ready draw rejected (0x%08lX)\n",
                    (unsigned long)hr);
    }
    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += out_count;

    if (g_pg.stats.draw_calls <= 5 || (g_pg.stats.draw_calls % 2000) == 0)
        fprintf(stderr, "[PGRAPH-D3D11] ready draw #%u: %u verts, nv2a mode %u,"
                        " prim %d, %u prims\n",
                g_pg.stats.draw_calls, out_count, nv2a_prim_mode,
                prim_type, prim_count);
    if (g_pg.stats.draw_calls <= 5) {
        /* The first vertices, so "it drew" can be told from "it drew
         * somewhere useful". Screen space for this title is 640x480. */
        uint32_t k, lim = out_count < 4 ? out_count : 4;
        for (k = 0; k < lim; k++)
            fprintf(stderr, "    v%u  x=%.1f y=%.1f  u=%.3f v=%.3f  argb=%08X\n",
                    k, out[k].x, out[k].y, out[k].u, out[k].v, out[k].color);
    }
}

/* A batch drawn with the title's vertex program run on the GPU (M4e). See
 * the header; the program, its constants and its HLSL come from
 * src/kernel/nv2a_vsh_cpu.c, the compile, cache and draw from
 * src/d3d/d3d8_extvs.c. */
extern int nv2a_vsh_emit_hlsl(char *out, int size, uint32_t *inputs_used);
extern uint32_t nv2a_vsh_program_hash(void);
extern const float *nv2a_vsh_constants(uint32_t *version);
extern int d3d8_Nv2aProgramDraw(uint32_t hash, const char *hlsl, uint32_t used,
                                const float *consts, uint32_t consts_version,
                                float zscale, const float texscale[2],
                                const float *verts, uint32_t nverts,
                                const uint16_t *idx, uint32_t nidx);

static uint32_t s_bad_prog[64];
static int s_bad_prog_n;

/* The next draw_program batches are point lists (NV2A primitive 1), drawn as
 * point sprites; `size` is SET_POINT_SIZE in pixels, used when the vertex
 * program writes no size of its own. */
/* Widescreen 2D placement for the next draws (nv2a_pb_exec.c, ws_place_batch). */
void pgraph_d3d11_set_placement(float kx, float bx)
{
    extern void d3d8_SetDrawPlacement(float kx, float bx);
    d3d8_SetDrawPlacement(kx, bx);
}

void pgraph_d3d11_set_point_mode(int on, float size)
{
    extern void d3d8_Nv2aPointMode(int on, float fixed_size);
    d3d8_Nv2aPointMode(on, size);
}

/* Whether the current program can take the GPU path (it has not failed). */
int pgraph_d3d11_program_usable(void)
{
    uint32_t hash = nv2a_vsh_program_hash();
    int k;
    for (k = 0; k < s_bad_prog_n; k++)
        if (s_bad_prog[k] == hash)
            return 0;
    return 1;
}

int pgraph_d3d11_draw_program(const float *verts, uint32_t nverts, uint32_t used,
                              const uint16_t *idx, uint32_t nidx,
                              float tex_u_scale, float tex_v_scale)
{
    uint32_t *bad = s_bad_prog;
    int bad_n = s_bad_prog_n;
    IDirect3DDevice8 *dev;
    const float *consts;
    uint32_t hash, version;
    float zscale, ts[2];
    int r, k;

    if (!g_pg.initialized || !(dev = xbox_GetD3DDevice()))
        return -1;
    hash = nv2a_vsh_program_hash();
    for (k = 0; k < bad_n; k++)
        if (bad[k] == hash)
            return -1;
    if (!ready_state(dev, &zscale))
        return 1;                           /* writes nothing: done */
    consts = nv2a_vsh_constants(&version);
    ts[0] = tex_u_scale;
    ts[1] = tex_v_scale;
    dev->lpVtbl->BeginScene(dev);
    r = d3d8_Nv2aProgramDraw(hash, NULL, used, consts, version, zscale, ts,
                             verts, nverts, idx, nidx);
    if (r == 0) {
        static char hlsl[96 * 1024];
        uint32_t u = 0;
        if (nv2a_vsh_emit_hlsl(hlsl, (int)sizeof hlsl, &u) < 0 || u != used) {
            r = -1;
        } else {
            r = d3d8_Nv2aProgramDraw(hash, hlsl, used, consts, version, zscale, ts,
                                     verts, nverts, idx, nidx);
        }
    }
    if (r < 0) {
        if (s_bad_prog_n < 64)
            s_bad_prog[s_bad_prog_n++] = hash;
        {
            static int told;
            if (told++ < 8)
                fprintf(stderr, "[PGRAPH-D3D11] vertex program %08X takes the CPU path%c",
                        hash, 10);
        }
        return -1;
    }
    g_pg.stats.draw_calls++;
    g_pg.stats.vertices_submitted += nidx;
    return 1;
}

void pgraph_d3d11_flush(void)
{
    if (g_pg.in_draw) {
        submit_draw();
        g_pg.in_draw = 0;
    }
    g_pg.stats.frames++;
}

void pgraph_d3d11_set_chyron_scroll(uint32_t pixels)
{
    g_pg.chyron_scroll_offset = (float)pixels;
}

void pgraph_d3d11_get_stats(PgraphD3D11Stats *out)
{
    if (out) *out = g_pg.stats;
}
