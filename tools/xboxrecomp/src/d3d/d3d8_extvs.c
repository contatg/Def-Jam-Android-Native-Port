/*
 * NV2A vertex programs drawn on the GPU (M4e).
 *
 * The push-buffer translator hands over a program it has already turned into
 * HLSL (src/kernel/nv2a_vsh_cpu.c, nv2a_vsh_emit_hlsl), its 192 constant
 * registers, and a batch of vertices -- one float4 per attribute the program
 * reads, in attribute order -- with a triangle-list index. This file compiles
 * and caches the program by the caller's hash, keeps the constants in a
 * buffer re-uploaded only when they changed, and draws through the D3D8
 * layer (d3d8_DrawIndexedExternalVS), so the pixel side is whatever the
 * current texture stages and render states make it.
 *
 * See docs/research/m4e-gpu-vertex-programs.md in the Def Jam port.
 */

#include "d3d8_internal.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "d3dcompiler.lib")

HRESULT d3d8_DrawIndexedExternalVS(const void *verts, UINT stride, UINT nverts,
                                   const uint16_t *idx, UINT nidx,
                                   ID3D11VertexShader *vs, ID3D11InputLayout *il,
                                   ID3D11Buffer *cb1, ID3D11Buffer *cb2);

void d3d8_SetExternalGS(ID3D11GeometryShader *gs);

#define EXTVS_CACHE 256

/* NV2A point sprites. Each point becomes a screen-aligned square of the size
 * the vertex program wrote to oPts (or the fixed SET_POINT_SIZE when it wrote
 * none), in the title's pixels, with the sprite's 0..1 coordinates on texture
 * stage 3 -- the stage the Xbox generates them for -- and everything else
 * constant across it. Def Jam draws its light glows this way. The struct is
 * the vertex shader's output, field for field (nv2a_vsh_emit_hlsl). */
static const char s_point_gs_src[] =
    "cbuffer VshFrame : register(b2) { float4 Screen; float4 TexScale; };\n"
    "struct VS_OUT {\n"
    "    float4 pos      : SV_POSITION;\n"
    "    float4 diffuse  : COLOR0;\n"
    "    float4 specular : COLOR1;\n"
    "    float3 tex0     : TEXCOORD0;\n"
    "    float3 tex1     : TEXCOORD1;\n"
    "    float3 tex2     : TEXCOORD2;\n"
    "    float3 tex3     : TEXCOORD3;\n"
    "    float  fog      : TEXCOORD4;\n"
    "    float4 viewpos  : TEXCOORD5;\n"
    "    float  psize    : TEXCOORD6;\n"
    "};\n"
    "[maxvertexcount(4)]\n"
    "void main(point VS_OUT i[1], inout TriangleStream<VS_OUT> s) {\n"
    "    float sz = i[0].psize > 0.0 ? i[0].psize : Screen.w;\n"
    "    if (sz <= 0.0 || i[0].pos.w <= 0.0) return;\n"
    /* TexScale.z: 1, or how much narrower a title pixel is across than down
     * on a widescreen canvas, so the square stays square. */
    "    float2 h = 0.5 * sz * Screen.xy * i[0].pos.w * float2(TexScale.z, 1.0);\n"
    "    static const float2 c[4] = { float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1) };\n"
    "    for (int k = 0; k < 4; k++) {\n"
    "        VS_OUT o = i[0];\n"
    "        o.pos.x += (c[k].x * 2.0 - 1.0) * h.x;\n"
    "        o.pos.y -= (c[k].y * 2.0 - 1.0) * h.y;\n"
    "        o.tex3 = float3(c[k], 0);\n"
    "        s.Append(o);\n"
    "    }\n"
    "}\n";

static ID3D11GeometryShader *s_point_gs;
static int   s_point_gs_failed;
static int   s_points;              /* the next draw is a point list */
static float s_point_size;          /* SET_POINT_SIZE, in pixels */

void d3d8_Nv2aPointMode(int on, float fixed_size)
{
    s_points = on;
    s_point_size = fixed_size;
}

static ID3D11GeometryShader *point_gs(void)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (s_point_gs || s_point_gs_failed)
        return s_point_gs;
    if (FAILED(D3DCompile(s_point_gs_src, strlen(s_point_gs_src), "nv2a_points", NULL, NULL,
                          "main", "gs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))
            || FAILED(ID3D11Device_CreateGeometryShader(d3d8_GetD3D11Device(),
                          ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code),
                          NULL, &s_point_gs))) {
        s_point_gs_failed = 1;
        fprintf(stderr, "[EXTVS] point-sprite geometry shader failed: %s\n",
                err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "create");
    } else {
        fprintf(stderr, "[EXTVS] point sprites: geometry shader ready\n");
    }
    if (err) ID3D10Blob_Release(err);
    if (code) ID3D10Blob_Release(code);
    return s_point_gs;
}

typedef struct {
    uint32_t hash, used;
    int      failed;                /* compile error: the caller falls back */
    ID3D11VertexShader *vs;
    ID3D11InputLayout  *il;
    uint64_t last_use;
} ExtVs;

static ExtVs          s_cache[EXTVS_CACHE];
static int            s_count;
static uint64_t       s_tick;
static ID3D11Buffer  *s_cb_consts;     /* b1: 192 float4 */
static ID3D11Buffer  *s_cb_frame;      /* b2: Screen, TexScale */
static uint32_t       s_consts_version = 0xFFFFFFFFu;

static int popcount16(uint32_t m)
{
    int n = 0;
    for (; m; m &= m - 1)
        n++;
    return n;
}

static ID3D11Buffer *make_cb(UINT bytes)
{
    D3D11_BUFFER_DESC bd;
    ID3D11Buffer *b = NULL;
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = bytes;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(d3d8_GetD3D11Device(), &bd, NULL, &b)))
        return NULL;
    return b;
}

static void upload(ID3D11Buffer *b, const void *data, size_t bytes)
{
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)b, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, data, bytes);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)b, 0);
    }
}

static ExtVs *lookup(uint32_t hash)
{
    int i;
    for (i = 0; i < s_count; i++)
        if (s_cache[i].hash == hash)
            return &s_cache[i];
    return NULL;
}

static ExtVs *compile(uint32_t hash, const char *hlsl, uint32_t used)
{
    ID3DBlob *code = NULL, *err = NULL;
    D3D11_INPUT_ELEMENT_DESC el[16];
    static char names[16][8];
    ExtVs *e;
    int slot, n = 0, a;
    HRESULT hr;

    if (s_count < EXTVS_CACHE) {
        slot = s_count++;
    } else {
        int j;
        slot = 0;
        for (j = 1; j < EXTVS_CACHE; j++)
            if (s_cache[j].last_use < s_cache[slot].last_use)
                slot = j;
        if (s_cache[slot].vs) ID3D11VertexShader_Release(s_cache[slot].vs);
        if (s_cache[slot].il) ID3D11InputLayout_Release(s_cache[slot].il);
    }
    e = &s_cache[slot];
    memset(e, 0, sizeof *e);
    e->hash = hash;
    e->used = used;

    hr = D3DCompile(hlsl, strlen(hlsl), "nv2a_vsh", NULL, NULL, "main", "vs_4_0",
                    D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
    if (FAILED(hr)) {
        static int told;
        e->failed = 1;
        if (told++ < 4)
            fprintf(stderr, "[EXTVS] program %08X did not compile: %s\n", hash,
                    err ? (const char *)ID3D10Blob_GetBufferPointer(err) : "?");
        if (err) ID3D10Blob_Release(err);
        return e;
    }
    if (err) ID3D10Blob_Release(err);
    hr = ID3D11Device_CreateVertexShader(d3d8_GetD3D11Device(),
                                         ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &e->vs);
    if (SUCCEEDED(hr)) {
        for (a = 0; a < 16; a++) {
            if (!((used >> a) & 1) && !(used == 0 && a == 0))
                continue;
            snprintf(names[a], sizeof names[a], "ATTR");
            memset(&el[n], 0, sizeof el[n]);
            el[n].SemanticName = names[a];
            el[n].SemanticIndex = (UINT)a;
            el[n].Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            el[n].AlignedByteOffset = (UINT)n * 16;
            el[n].InputSlotClass = D3D11_INPUT_PER_VERTEX_DATA;
            n++;
        }
        hr = ID3D11Device_CreateInputLayout(d3d8_GetD3D11Device(), el, (UINT)n,
                                            ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), &e->il);
    }
    ID3D10Blob_Release(code);
    if (FAILED(hr)) {
        static int told;
        e->failed = 1;
        if (told++ < 4)
            fprintf(stderr, "[EXTVS] program %08X: shader or layout creation failed 0x%08lX\n",
                    hash, (unsigned long)hr);
    } else {
        static unsigned compiled;
        if (compiled++ < 8 || (compiled & (compiled - 1)) == 0)
            fprintf(stderr, "[EXTVS] compiled program %08X (%d inputs) (#%u)\n",
                    hash, n, compiled);
    }
    return e;
}

/* Returns 1 if drawn, 0 if the program is not cached and hlsl was NULL (call
 * again with it), -1 if this program cannot be drawn here (fall back). */
int d3d8_Nv2aProgramDraw(uint32_t hash, const char *hlsl, uint32_t used,
                         const float *consts, uint32_t consts_version,
                         float zscale, const float texscale[2],
                         const float *verts, uint32_t nverts,
                         const uint16_t *idx, uint32_t nidx)
{
    ExtVs *e;
    float frame[8];

    if (!d3d8_GetD3D11Device())
        return -1;
    if (!s_cb_consts) {
        s_cb_consts = make_cb(192 * 16);
        s_cb_frame = make_cb(32);
        if (!s_cb_consts || !s_cb_frame)
            return -1;
    }
    e = lookup(hash);
    if (!e) {
        if (!hlsl)
            return 0;
        e = compile(hash, hlsl, used);
    }
    if (e->failed || !e->vs || !e->il)
        return -1;
    e->last_use = ++s_tick;

    if (consts_version != s_consts_version) {
        upload(s_cb_consts, consts, 192 * 16);
        s_consts_version = consts_version;
    }
    {
        /* A widescreen placement for this draw: clip x becomes x * kx + bx
         * (the program's epilogue computes x * Screen.x - TexScale.w). */
        float kx, bx;
        d3d8_GetDrawPlacement(&kx, &bx);
        frame[0] = 2.0f * kx / (float)d3d8_GetBackbufferWidth();
        frame[7] = kx - bx;
    }
    frame[1] = 2.0f / (float)d3d8_GetBackbufferHeight();
    frame[2] = zscale;
    frame[3] = s_points ? s_point_size : 0.0f;
    frame[4] = texscale[0];
    frame[5] = texscale[1];
    frame[6] = d3d8_GetCanvasScaleY() / d3d8_GetCanvasScaleX();   /* point sprites: 1 unless widened */
    upload(s_cb_frame, frame, sizeof frame);

    if (s_points) {
        ID3D11GeometryShader *gs = point_gs();
        HRESULT hr;
        if (!gs)
            return 1;                       /* cannot draw points: skip, do not fall back */
        d3d8_SetExternalGS(gs);
        hr = d3d8_DrawIndexedExternalVS(verts, (UINT)popcount16(used ? used : 1) * 16,
                                        nverts, idx, nidx, e->vs, e->il,
                                        s_cb_consts, s_cb_frame);
        d3d8_SetExternalGS(NULL);
        return SUCCEEDED(hr) ? 1 : -1;
    }
    return SUCCEEDED(d3d8_DrawIndexedExternalVS(verts, (UINT)popcount16(used ? used : 1) * 16,
                                                nverts, idx, nidx, e->vs, e->il,
                                                s_cb_consts, s_cb_frame)) ? 1 : -1;
}
