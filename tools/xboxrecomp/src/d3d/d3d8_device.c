/**
 * D3D8→D3D11 Compatibility Device Implementation
 *
 * Implements the Xbox D3D8 IDirect3DDevice8 interface using D3D11.
 * The game's translated RenderWare code calls D3D8 methods through
 * COM vtables; this layer translates those calls to D3D11 equivalents.
 *
 * Architecture:
 * - D3D11 device and swap chain created during initialization
 * - Render state tracking: D3D8 states mapped to D3D11 state objects
 * - Texture/buffer management: D3D8 resource handles wrap D3D11 resources
 * - Fixed-function pipeline: emulated via D3D11 shaders (the Xbox D3D8
 *   pipeline is configurable but not fully programmable)
 *
 * Build: Requires Windows SDK with d3d11.h and dxgi.h
 */

#include "d3d8_internal.h"
#include "d3d8_present.h"
#include "d3d8_readback.h"
#include <stdio.h>
#include <string.h>

/* ================================================================
 * Internal device state
 * ================================================================ */

/* Maximum tracked render states, texture stages, and transforms */
#define MAX_RENDER_STATES    256
#define MAX_TEXTURE_STAGES   4
#define MAX_TSS_STATES       32
#define MAX_TRANSFORMS       512
#define MAX_LIGHTS           8

typedef struct D3D8DeviceState {
    /* D3D11 objects */
    ID3D11Device            *d3d11_device;
    ID3D11DeviceContext     *d3d11_context;
    IDXGISwapChain          *swap_chain;

    /* Default render targets */
    ID3D11RenderTargetView  *default_rtv;
    ID3D11DepthStencilView  *default_dsv;
    ID3D11Texture2D         *default_depth;

    /* Window */
    HWND                    hwnd;
    UINT                    width;          /* the back buffer, in pixels */
    UINT                    height;
    UINT                    logical_width;  /* what the title asked for */
    UINT                    logical_height;
    UINT                    render_scale;   /* width / logical_width */
    D3DFORMAT               backbuffer_format;

    /* State tracking */
    DWORD                   render_states[MAX_RENDER_STATES];
    DWORD                   tss[MAX_TEXTURE_STAGES][MAX_TSS_STATES];
    D3DMATRIX               transforms[MAX_TRANSFORMS];
    D3DVIEWPORT8            viewport;
    D3DMATERIAL8            material;
    D3DLIGHT8               lights[MAX_LIGHTS];
    BOOL                    light_enable[MAX_LIGHTS];

    /* Current shader/FVF */
    DWORD                   vertex_shader;
    DWORD                   pixel_shader;

    /* Scene state */
    BOOL                    in_scene;

    /* Reference count */
    LONG                    ref_count;
} D3D8DeviceState;

/* Global device instance (Xbox has a single D3D device) */
static D3D8DeviceState g_device_state;
static IDirect3DDevice8 g_device;
static BOOL g_device_initialized = FALSE;

/* Current resource bindings */
static IDirect3DVertexBuffer8 *g_cur_vb = NULL;
static UINT                    g_cur_vb_stride = 0;
static IDirect3DIndexBuffer8  *g_cur_ib = NULL;
static UINT                    g_cur_ib_base_vertex = 0;
static IDirect3DBaseTexture8  *g_cur_textures[4] = { NULL };

/* Bound offscreen render target / depth stencil (NULL = default) */
static D3D8Surface *g_cur_rt = NULL;
static D3D8Surface *g_cur_ds = NULL;

/* Palettized texture palettes (256 ARGB entries per stage) */
#define D3D8_PALETTE_ENTRIES 256
static DWORD g_palettes[D3D8_MAX_PALETTES][D3D8_PALETTE_ENTRIES];
static BOOL  g_palettes_initialized = FALSE;

static void d3d8_palettes_init(void)
{
    int p, i;
    if (g_palettes_initialized) return;
    for (p = 0; p < D3D8_MAX_PALETTES; p++)
        for (i = 0; i < D3D8_PALETTE_ENTRIES; i++)
            g_palettes[p][i] = 0xFF000000u | ((DWORD)i * 0x010101u); /* gray ramp */
    g_palettes_initialized = TRUE;
}

void d3d8_SetPalette(DWORD stage, const DWORD *entries)
{
    d3d8_palettes_init();
    if (stage >= D3D8_MAX_PALETTES) return;
    if (entries)
        memcpy(g_palettes[stage], entries, sizeof(g_palettes[stage]));
    else {
        for (int i = 0; i < D3D8_PALETTE_ENTRIES; i++)
            g_palettes[stage][i] = 0xFF000000u | ((DWORD)i * 0x010101u);
    }
}

const DWORD *d3d8_GetPalette(DWORD stage)
{
    d3d8_palettes_init();
    return (stage < D3D8_MAX_PALETTES) ? g_palettes[stage] : g_palettes[0];
}

/* Forward declarations */
static const IDirect3DDevice8Vtbl g_device_vtbl;
static void up_ring_shutdown(void);

static HRESULT d3d8_present(void)
{
    HRESULT hr = d3d8_gamma_begin(g_device_state.default_rtv);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Gamma presentation failed: 0x%08lX\n", hr);
        return hr;
    }
    {
        /* No wait for the host's vertical blank unless RECOMP_PRESENT_VSYNC=1.
         * When this presents a push buffer's flips, a wait here is a wait in
         * the GPU executor, and the executor already paces the title's flips
         * at 60 Hz (FLIP_STALL, RECOMP_FLIP_HZ). With both, a Def Jam fight
         * presented 13-15 frames a second; without the wait, 20-23. The intro
         * movie once stalled without it (2 start-ups of 4, before the guest
         * clocks moved to QPC); 6 of 6 reach the title now. */
        static int env_vsync = -1;
        int vsync = d3d8_present_sync_interval();   /* the host's choice, if it made one */
        if (env_vsync < 0) {
            const char *e = getenv("RECOMP_PRESENT_VSYNC");
            env_vsync = (e && *e == '1') ? 1 : 0;
        }
        if (vsync < 0)
            vsync = env_vsync;
        /* With scaled presentation the title's target is not the swap chain's
         * buffer: draw it into the window first (after the gamma pass, so the
         * window shows the ramped picture). */
        hr = d3d8_present_blit();
        if (FAILED(hr))
            fprintf(stderr, "D3D8: Scaled presentation failed: 0x%08lX\n", hr);
        hr = IDXGISwapChain_Present(g_device_state.swap_chain, (UINT)vsync, 0);
        d3d8_present_trace_pacing();
    }
    d3d8_gamma_end(g_device_state.default_rtv);
    return hr;
}

/* ================================================================
 * Public frame pump (called from recompiled game code)
 * ================================================================ */
void d3d8_PresentFrame(void)
{
    /* Pump Windows messages */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) ExitProcess(0);
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    /* Present the backbuffer (VSync = 1) */
    if (g_device_state.swap_chain)
        d3d8_present();
}

/* ================================================================
 * Internal accessors (used by d3d8_resources/shaders/states)
 * ================================================================ */

IDirect3DDevice8    *d3d8_GetDevice(void) { return &g_device; }
ID3D11Device        *d3d8_GetD3D11Device(void) { return g_device_state.d3d11_device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return g_device_state.d3d11_context; }
IDXGISwapChain      *d3d8_GetSwapChain(void) { return g_device_state.swap_chain; }
ID3D11RenderTargetView *d3d8_GetDefaultRTV(void) { return g_device_state.default_rtv; }
HWND                 d3d8_GetHWND(void) { return g_device_state.hwnd; }
/* The title's screen size: pre-transformed vertices and full-screen quads are
 * in these units whatever the render scale. */
UINT                 d3d8_GetBackbufferWidth(void) { return g_device_state.logical_width; }
UINT                 d3d8_GetBackbufferHeight(void) { return g_device_state.logical_height; }
UINT                 d3d8_GetRenderScale(void) { return g_device_state.render_scale ? g_device_state.render_scale : 1; }
static float g_place_kx = 1.0f, g_place_bx = 0.0f;
void d3d8_SetDrawPlacement(float kx, float bx) { g_place_kx = kx; g_place_bx = bx; }
void d3d8_GetDrawPlacement(float *kx, float *bx) { *kx = g_place_kx; *bx = g_place_bx; }
float d3d8_GetCanvasScaleX(void)
{
    return g_device_state.logical_width ? (float)g_device_state.width / (float)g_device_state.logical_width
                                        : (float)d3d8_GetRenderScale();
}
float d3d8_GetCanvasScaleY(void)
{
    return g_device_state.logical_height ? (float)g_device_state.height / (float)g_device_state.logical_height
                                         : (float)d3d8_GetRenderScale();
}
const DWORD         *d3d8_GetRenderStates(void) { return g_device_state.render_states; }
const DWORD         *d3d8_GetTSS(DWORD stage) { return (stage < MAX_TEXTURE_STAGES) ? g_device_state.tss[stage] : NULL; }
IDirect3DBaseTexture8 *d3d8_GetStageTexture(DWORD stage) { return (stage < 4) ? g_cur_textures[stage] : NULL; }
const D3DMATRIX     *d3d8_GetTransform(D3DTRANSFORMSTATETYPE type) {
    return ((DWORD)type < MAX_TRANSFORMS) ? &g_device_state.transforms[(DWORD)type] : NULL;
}

const D3DLIGHT8     *d3d8_GetLight(DWORD index) {
    return (index < MAX_LIGHTS) ? &g_device_state.lights[index] : NULL;
}

BOOL                 d3d8_GetLightEnable(DWORD index) {
    return (index < MAX_LIGHTS) ? g_device_state.light_enable[index] : FALSE;
}

const D3DMATERIAL8  *d3d8_GetMaterial(void) {
    return &g_device_state.material;
}

DWORD                d3d8_GetCurrentFVF(void) {
    return g_cur_vb ? ((D3D8VertexBuffer *)g_cur_vb)->fvf : 0;
}

UINT                 d3d8_GetNumLights(void) {
    return MAX_LIGHTS;
}

/* ================================================================
 * D3D11 initialization helpers
 * ================================================================ */

#ifdef _DEBUG
#include <d3d11sdklayers.h>
/* The debug layer breaks on a corruption-class message by raising exception
 * 0x87D, which with no debugger attached ends the process without a word.
 * Print what it had to say and carry on: the message is the diagnosis. */
static ID3D11InfoQueue *g_info_queue;

static LONG CALLBACK d3d11_debug_break_handler(EXCEPTION_POINTERS *ep)
{
    UINT64 n, i, first;
    static int reports;
    if (ep->ExceptionRecord->ExceptionCode != 0x87D || !g_info_queue)
        return EXCEPTION_CONTINUE_SEARCH;
    n = ID3D11InfoQueue_GetNumStoredMessages(g_info_queue);
    first = n > 6 ? n - 6 : 0;
    if (reports++ < 8) {
        fprintf(stderr, "[D3D11-DEBUG] debug layer break (thread %lu); last %u of %u message(s):\n",
                GetCurrentThreadId(), (unsigned)(n - first), (unsigned)n);
        for (i = first; i < n; i++) {
            SIZE_T len = 0;
            D3D11_MESSAGE *m;
            ID3D11InfoQueue_GetMessage(g_info_queue, i, NULL, &len);
            m = (D3D11_MESSAGE *)malloc(len);
            if (m && SUCCEEDED(ID3D11InfoQueue_GetMessage(g_info_queue, i, m, &len)))
                fprintf(stderr, "[D3D11-DEBUG]   sev %d id %d: %.*s\n", (int)m->Severity,
                        (int)m->ID, (int)m->DescriptionByteLength, m->pDescription);
            free(m);
        }
        fflush(stderr);
    }
    ID3D11InfoQueue_ClearStoredMessages(g_info_queue);
    return EXCEPTION_CONTINUE_EXECUTION;
}
#endif

static HRESULT d3d11_create_device_and_swap_chain(
    D3D8DeviceState *state,
    D3DPRESENT_PARAMETERS *pp)
{
    DXGI_SWAP_CHAIN_DESC scd;
    D3D_FEATURE_LEVEL feature_level;
    UINT create_flags = 0;
    HRESULT hr;

#ifdef _DEBUG
    create_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    /* RECOMP_RENDER_SCALE=N (1-4, default 2): the back buffer is N times the
     * title's size in each direction and everything rasterises at that
     * resolution, while the title keeps working in its own units (the getters
     * above; the viewport covers the whole buffer). Presented into a window of
     * the title's size it is supersampled -- the edge smoothing the console
     * gets from rendering a fight's 3D at twice the width (surface format
     * 0x1128, anti-aliasing mode 1) and filtering it down, which is not
     * modelled -- and in a larger window it is simply sharper. */
    {
        const char *e = getenv("RECOMP_RENDER_SCALE");
        int n = e && *e ? atoi(e) : 2;
        if (d3d8_present_render_scale())
            n = (int)d3d8_present_render_scale();
        state->render_scale = (UINT)(n < 1 ? 1 : n > 4 ? 4 : n);
    }
    state->logical_width = pp->BackBufferWidth ? pp->BackBufferWidth : 640;
    state->logical_height = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    memset(&scd, 0, sizeof(scd));
    scd.BufferCount = pp->BackBufferCount ? pp->BackBufferCount : 1;
    scd.BufferDesc.Width = state->logical_width * state->render_scale;
    scd.BufferDesc.Height = state->logical_height * state->render_scale;
    if (d3d8_present_scaling()) {
        /* The swap chain follows the window (0 = its client area); the title
         * renders into a target of its own, created with the render targets. */
        scd.BufferDesc.Width = 0;
        scd.BufferDesc.Height = 0;
    }
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = pp->hDeviceWindow;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = pp->Windowed;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = E_FAIL;
    if (d3d8_present_scaling()) {
        /* Flip model: a borderless window is presented without a copy through
         * the desktop compositor, which is what exclusive full screen used to
         * be needed for, and a sync interval waits for the display properly.
         * Windows 10 and later; older systems fall through to the blit model. */
        DXGI_SWAP_CHAIN_DESC flip = scd;
        flip.BufferCount = 2;
        flip.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, create_flags,
                                           NULL, 0, D3D11_SDK_VERSION, &flip, &state->swap_chain,
                                           &state->d3d11_device, &feature_level,
                                           &state->d3d11_context);
        if (FAILED(hr))
            fprintf(stderr, "D3D8: Flip-model swap chain unavailable (0x%08lX); using the blit model\n", hr);
    }
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(
            NULL,
            D3D_DRIVER_TYPE_HARDWARE,
            NULL,
            create_flags,
            NULL, 0,
            D3D11_SDK_VERSION,
            &scd,
            &state->swap_chain,
            &state->d3d11_device,
            &feature_level,
            &state->d3d11_context
        );

    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Failed to create D3D11 device: 0x%08lX\n", hr);
        return hr;
    }

#ifdef _DEBUG
    if (SUCCEEDED(ID3D11Device_QueryInterface(state->d3d11_device, &IID_ID3D11InfoQueue,
                                              (void **)&g_info_queue)))
        AddVectoredExceptionHandler(1, d3d11_debug_break_handler);
#endif

    state->hwnd = pp->hDeviceWindow;
    {
        /* A widescreen canvas widens only the title's own target (scaling mode);
         * the title's coordinates keep their range and spread over it. */
        UINT num, den;
        d3d8_present_canvas_aspect(&num, &den);
        state->width = d3d8_canvas_width(state->logical_width, state->logical_height,
                                         state->render_scale, num, den);
        if (state->width != state->logical_width * state->render_scale)
            fprintf(stderr, "D3D8: widescreen canvas %u:%u, target %ux%u for a %ux%u title\n",
                    num, den, state->width, state->logical_height * state->render_scale,
                    state->logical_width, state->logical_height);
    }
    state->height = state->logical_height * state->render_scale;

    return S_OK;
}

static HRESULT d3d11_create_render_targets(D3D8DeviceState *state)
{
    ID3D11Texture2D *back_buffer = NULL;
    D3D11_TEXTURE2D_DESC depth_desc;
    HRESULT hr;

    if (d3d8_present_scaling()) {
        /* The default target is a texture of the title's size; each present
         * draws it into the swap chain's buffer (d3d8_present.c). */
        hr = d3d8_present_create_game_target(state->width, state->height,
                                             &state->default_rtv);
        if (FAILED(hr)) return hr;
    } else {
        /* Create render target view from swap chain back buffer */
        hr = IDXGISwapChain_GetBuffer(state->swap_chain, 0,
                                       &IID_ID3D11Texture2D,
                                       (void **)&back_buffer);
        if (FAILED(hr)) return hr;

        hr = ID3D11Device_CreateRenderTargetView(state->d3d11_device,
                                                  (ID3D11Resource *)back_buffer,
                                                  NULL, &state->default_rtv);
        ID3D11Texture2D_Release(back_buffer);
        if (FAILED(hr)) return hr;
    }

    /* Create depth stencil */
    memset(&depth_desc, 0, sizeof(depth_desc));
    depth_desc.Width = state->width;
    depth_desc.Height = state->height;
    depth_desc.MipLevels = 1;
    depth_desc.ArraySize = 1;
    depth_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth_desc.SampleDesc.Count = 1;
    depth_desc.SampleDesc.Quality = 0;
    depth_desc.Usage = D3D11_USAGE_DEFAULT;
    depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(state->d3d11_device, &depth_desc,
                                       NULL, &state->default_depth);
    if (FAILED(hr)) return hr;

    hr = ID3D11Device_CreateDepthStencilView(state->d3d11_device,
                                              (ID3D11Resource *)state->default_depth,
                                              NULL, &state->default_dsv);
    if (FAILED(hr)) return hr;

    /* Bind default render targets */
    ID3D11DeviceContext_OMSetRenderTargets(state->d3d11_context, 1,
                                            &state->default_rtv,
                                            state->default_dsv);

    return S_OK;
}

static void d3d8_init_default_states(D3D8DeviceState *state)
{
    /* Set Xbox D3D8 default render states */
    memset(state->render_states, 0, sizeof(state->render_states));
    state->render_states[D3DRS_ZENABLE]           = 1;
    state->render_states[D3DRS_FILLMODE]          = D3DFILL_SOLID;
    state->render_states[D3DRS_SHADEMODE]         = 2; /* D3DSHADE_GOURAUD */
    state->render_states[D3DRS_ZWRITEENABLE]      = TRUE;
    state->render_states[D3DRS_ALPHATESTENABLE]    = FALSE;
    state->render_states[D3DRS_SRCBLEND]          = D3DBLEND_ONE;
    state->render_states[D3DRS_DESTBLEND]         = D3DBLEND_ZERO;
    state->render_states[D3DRS_CULLMODE]          = D3DCULL_CCW;
    state->render_states[D3DRS_ZFUNC]             = D3DCMP_LESSEQUAL;
    state->render_states[D3DRS_ALPHAREF]          = 0;
    state->render_states[D3DRS_ALPHAFUNC]         = D3DCMP_ALWAYS;
    state->render_states[D3DRS_ALPHABLENDENABLE]   = FALSE;
    state->render_states[D3DRS_FOGENABLE]         = FALSE;
    state->render_states[D3DRS_STENCILENABLE]     = FALSE;
    state->render_states[D3DRS_COLORWRITEENABLE]  = 0x0F;

    /* Default viewport */
    state->viewport.X = 0;
    state->viewport.Y = 0;
    state->viewport.Width = state->width;
    state->viewport.Height = state->height;
    state->viewport.MinZ = 0.0f;
    state->viewport.MaxZ = 1.0f;

    /* Default texture stage states:
     * By default each stage reads its own texcoord set (0,1,2,3). */
    memset(state->tss, 0, sizeof(state->tss));
    for (int s = 0; s < MAX_TEXTURE_STAGES; s++)
        state->tss[s][D3DTSS_TEXCOORDINDEX] = (DWORD)s;

    /* Identity matrices */
    for (int i = 0; i < MAX_TRANSFORMS; i++) {
        memset(&state->transforms[i], 0, sizeof(D3DMATRIX));
        state->transforms[i]._11 = 1.0f;
        state->transforms[i]._22 = 1.0f;
        state->transforms[i]._33 = 1.0f;
        state->transforms[i]._44 = 1.0f;
    }

    state->vertex_shader = 0;
    state->pixel_shader = 0;
    state->in_scene = FALSE;
}

/* ================================================================
 * IDirect3DDevice8 method implementations
 * ================================================================ */

static HRESULT __stdcall dev_QueryInterface(IDirect3DDevice8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall dev_AddRef(IDirect3DDevice8 *self)
{
    (void)self;
    return InterlockedIncrement(&g_device_state.ref_count);
}

static ULONG __stdcall dev_Release(IDirect3DDevice8 *self)
{
    (void)self;
    LONG ref = InterlockedDecrement(&g_device_state.ref_count);
    if (ref <= 0) {
        /* Cleanup subsystems first */
        up_ring_shutdown();
        d3d8_readback_reset();
        d3d8_gamma_shutdown();
        d3d8_present_shutdown();
        d3d8_vsh_shutdown();
        d3d8_combiners_shutdown();
        d3d8_states_shutdown();
        d3d8_shaders_shutdown();

        /* Cleanup D3D11 resources */
        D3D8DeviceState *s = &g_device_state;
        if (s->default_dsv) { ID3D11DepthStencilView_Release(s->default_dsv); s->default_dsv = NULL; }
        if (s->default_depth) { ID3D11Texture2D_Release(s->default_depth); s->default_depth = NULL; }
        if (s->default_rtv) { ID3D11RenderTargetView_Release(s->default_rtv); s->default_rtv = NULL; }
        if (s->swap_chain) { IDXGISwapChain_Release(s->swap_chain); s->swap_chain = NULL; }
        if (s->d3d11_context) { ID3D11DeviceContext_Release(s->d3d11_context); s->d3d11_context = NULL; }
        if (s->d3d11_device) { ID3D11Device_Release(s->d3d11_device); s->d3d11_device = NULL; }
        g_device_initialized = FALSE;
    }
    return (ULONG)ref;
}

static HRESULT __stdcall dev_GetDirect3D(IDirect3DDevice8 *self, IDirect3D8 **ppD3D8)
{
    (void)self; (void)ppD3D8;
    /* TODO: return the factory */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_GetDeviceCaps(IDirect3DDevice8 *self, void *pCaps)
{
    (void)self; (void)pCaps;
    /* TODO: fill with Xbox NV2A capabilities */
    return S_OK;
}

static HRESULT __stdcall dev_GetDisplayMode(IDirect3DDevice8 *self, void *pMode)
{
    (void)self; (void)pMode;
    return S_OK;
}

static HRESULT __stdcall dev_GetCreationParameters(IDirect3DDevice8 *self, void *pParams)
{
    (void)self; (void)pParams;
    return S_OK;
}

static HRESULT __stdcall dev_Reset(IDirect3DDevice8 *self, D3DPRESENT_PARAMETERS *pPP)
{
    (void)self; (void)pPP;
    /* TODO: resize swap chain */
    return S_OK;
}

static DWORD g_d3d_begin_count = 0;
static DWORD g_d3d_end_count = 0;
static DWORD g_d3d_clear_count = 0;
static DWORD g_d3d_draw_count = 0;
static DWORD g_d3d_settransform_count = 0;
static DWORD g_d3d_setrs_count = 0;
static DWORD g_d3d_settexture_count = 0;

static HRESULT __stdcall dev_Present(IDirect3DDevice8 *self, const RECT *src, const RECT *dst, HWND hWnd, void *pDirty)
{
    static DWORD frame_count = 0;
    static DWORD last_tick = 0;
    (void)self; (void)src; (void)dst; (void)hWnd; (void)pDirty;

    frame_count++;
    DWORD now = GetTickCount();
    if (last_tick == 0) last_tick = now;
    if (now - last_tick >= 2000) {
        fprintf(stderr, "  [D3D] %.1fs: %u present (%.1f fps), %u begin, %u end, "
                "%u clear, %u draw, %u xform, %u rs, %u tex\n",
                (now - last_tick) / 1000.0, frame_count,
                frame_count * 1000.0 / (now - last_tick),
                g_d3d_begin_count, g_d3d_end_count,
                g_d3d_clear_count, g_d3d_draw_count,
                g_d3d_settransform_count, g_d3d_setrs_count,
                g_d3d_settexture_count);
        fflush(stderr);
        frame_count = 0;
        g_d3d_begin_count = g_d3d_end_count = 0;
        g_d3d_clear_count = g_d3d_draw_count = 0;
        g_d3d_settransform_count = g_d3d_setrs_count = 0;
        g_d3d_settexture_count = 0;
        last_tick = now;
    }

    /* Pump Windows messages: the game's internal main loop drives rendering,
     * so our external message pump never runs. Process messages here to keep
     * the window responsive and handle input. */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return d3d8_present();
}

static HRESULT __stdcall dev_GetBackBuffer(IDirect3DDevice8 *self, INT iBackBuffer, DWORD Type, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)iBackBuffer; (void)Type;
    ID3D11Texture2D *back_buffer = NULL;
    HRESULT hr;

    if (!ppSurface) return E_INVALIDARG;

    if (d3d8_present_scaling() && d3d8_present_game_texture()) {
        /* What the title drew, whatever the window's size. */
        back_buffer = d3d8_present_game_texture();
        ID3D11Texture2D_AddRef(back_buffer);
        hr = S_OK;
    } else {
        hr = IDXGISwapChain_GetBuffer(g_device_state.swap_chain, 0,
                                       &IID_ID3D11Texture2D,
                                       (void **)&back_buffer);
    }
    if (FAILED(hr)) return hr;

    *ppSurface = d3d8_surface_create(back_buffer, 0, 0,
                                     g_device_state.width,
                                     g_device_state.height,
                                     D3DFMT_X8R8G8B8,
                                     D3DPOOL_DEFAULT, 0,
                                     D3DMULTISAMPLE_NONE, NULL, 0);
    ID3D11Texture2D_Release(back_buffer);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_BeginScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = TRUE;
    g_d3d_begin_count++;
    return S_OK;
}

static HRESULT __stdcall dev_EndScene(IDirect3DDevice8 *self)
{
    (void)self;
    g_device_state.in_scene = FALSE;
    g_d3d_end_count++;
    return S_OK;
}

static HRESULT __stdcall dev_Clear(IDirect3DDevice8 *self, DWORD Count, const D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{
    (void)self; (void)Count; (void)pRects; (void)Stencil;
    g_d3d_clear_count++;

    /* Clear the currently bound targets (default if none set) */
    ID3D11RenderTargetView *rtv = g_cur_rt ? g_cur_rt->rtv : g_device_state.default_rtv;
    ID3D11DepthStencilView *dsv = g_cur_ds ? g_cur_ds->dsv : g_device_state.default_dsv;

    if ((Flags & D3DCLEAR_TARGET) && rtv) {
        float clear_color[4] = {
            ((Color >> 16) & 0xFF) / 255.0f,  /* R */
            ((Color >>  8) & 0xFF) / 255.0f,  /* G */
            ((Color >>  0) & 0xFF) / 255.0f,  /* B */
            ((Color >> 24) & 0xFF) / 255.0f,  /* A */
        };
        ID3D11DeviceContext_ClearRenderTargetView(g_device_state.d3d11_context,
                                                   rtv,
                                                   clear_color);
    }

    if ((Flags & (D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL)) && dsv) {
        UINT clear_flags = 0;
        if (Flags & D3DCLEAR_ZBUFFER) clear_flags |= D3D11_CLEAR_DEPTH;
        if (Flags & D3DCLEAR_STENCIL) clear_flags |= D3D11_CLEAR_STENCIL;

        ID3D11DeviceContext_ClearDepthStencilView(g_device_state.d3d11_context,
                                                    dsv,
                                                    clear_flags, Z, (UINT8)Stencil);
    }

    return S_OK;
}

static HRESULT __stdcall dev_SetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, const D3DMATRIX *pMatrix)
{
    (void)self;
    g_d3d_settransform_count++;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        g_device_state.transforms[(DWORD)State] = *pMatrix;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTransform(IDirect3DDevice8 *self, D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    (void)self;
    if ((DWORD)State < MAX_TRANSFORMS && pMatrix) {
        *pMatrix = g_device_state.transforms[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD Value)
{
    (void)self;
    g_d3d_setrs_count++;
    if ((DWORD)State < MAX_RENDER_STATES) {
        g_device_state.render_states[(DWORD)State] = Value;
    }
    /* Mark combiner state dirty if any PS register combiner state changed */
    if ((DWORD)State >= D3DRS_PSALPHAINPUTS0 && (DWORD)State <= D3DRS_PSINPUTTEXTURE) {
        d3d8_combiners_mark_dirty();
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderState(IDirect3DDevice8 *self, D3DRENDERSTATETYPE State, DWORD *pValue)
{
    (void)self;
    if ((DWORD)State < MAX_RENDER_STATES && pValue) {
        *pValue = g_device_state.render_states[(DWORD)State];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES) {
        g_device_state.tss[Stage][(DWORD)Type] = Value;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTextureStageState(IDirect3DDevice8 *self, DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    (void)self;
    if (Stage < MAX_TEXTURE_STAGES && (DWORD)Type < MAX_TSS_STATES && pValue) {
        *pValue = g_device_state.tss[Stage][(DWORD)Type];
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 *pTexture)
{
    (void)self;
    g_d3d_settexture_count++;
    if (Stage >= 4) return E_INVALIDARG;
    g_cur_textures[Stage] = pTexture;

    /* Bind SRV to pixel shader */
    if (pTexture) {
        ID3D11ShaderResourceView *srv = d3d8_base_srv(pTexture);
        /* P8 textures bake a palette; record which stage palette they use
         * so SetPalette can re-bake them later. */
        if (d3d8_format_is_palettized(d3d8_base_format(pTexture)))
            d3d8_base_set_palette(pTexture, Stage);
        if (srv) {
            ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
                Stage, 1, &srv);
        }
        /* Mark texture stage as active */
        if (g_device_state.tss[Stage][D3DTSS_COLOROP] == D3DTOP_DISABLE)
            g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_MODULATE;
    } else {
        ID3D11ShaderResourceView *null_srv = NULL;
        ID3D11DeviceContext_PSSetShaderResources(g_device_state.d3d11_context,
            Stage, 1, &null_srv);
        g_device_state.tss[Stage][D3DTSS_COLOROP] = D3DTOP_DISABLE;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetTexture(IDirect3DDevice8 *self, DWORD Stage, IDirect3DBaseTexture8 **ppTexture)
{
    (void)self;
    if (!ppTexture || Stage >= MAX_TEXTURE_STAGES) return E_INVALIDARG;
    *ppTexture = g_cur_textures[Stage];
    if (*ppTexture) IDirect3DBaseTexture8_AddRef(*ppTexture);
    return S_OK;
}

static HRESULT __stdcall dev_SetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 *pStreamData, UINT Stride)
{
    (void)self;
    if (StreamNumber != 0) return S_OK; /* Only stream 0 supported */
    g_cur_vb = pStreamData;
    g_cur_vb_stride = Stride;

    if (pStreamData) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)pStreamData;
        UINT offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &Stride, &offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetStreamSource(IDirect3DDevice8 *self, UINT StreamNumber, IDirect3DVertexBuffer8 **ppStreamData, UINT *pStride)
{
    (void)self;
    if (!ppStreamData || StreamNumber != 0) return E_INVALIDARG;
    *ppStreamData = g_cur_vb;
    if (*ppStreamData) (*ppStreamData)->lpVtbl->AddRef(*ppStreamData);
    if (pStride) *pStride = g_cur_vb_stride;
    return S_OK;
}

static HRESULT __stdcall dev_SetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 *pIndexData, UINT BaseVertexIndex)
{
    (void)self;
    g_cur_ib = pIndexData;
    g_cur_ib_base_vertex = BaseVertexIndex;

    if (pIndexData) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)pIndexData;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32)
            ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetIndices(IDirect3DDevice8 *self, IDirect3DIndexBuffer8 **ppIndexData, UINT *pBaseVertexIndex)
{
    (void)self;
    if (!ppIndexData) return E_INVALIDARG;
    *ppIndexData = g_cur_ib;
    if (*ppIndexData) (*ppIndexData)->lpVtbl->AddRef(*ppIndexData);
    if (pBaseVertexIndex) *pBaseVertexIndex = g_cur_ib_base_vertex;
    return S_OK;
}

static D3D11_PRIMITIVE_TOPOLOGY map_primitive_type(D3DPRIMITIVETYPE pt, UINT count, UINT *out_count)
{
    switch (pt) {
    case D3DPT_TRIANGLELIST:  *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_TRIANGLESTRIP: *out_count = count + 2; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    case D3DPT_TRIANGLEFAN:   *out_count = count * 3; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    case D3DPT_LINELIST:      *out_count = count * 2; return D3D11_PRIMITIVE_TOPOLOGY_LINELIST;
    case D3DPT_LINESTRIP:     *out_count = count + 1; return D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP;
    case D3DPT_POINTLIST:     *out_count = count;     return D3D11_PRIMITIVE_TOPOLOGY_POINTLIST;
    case D3DPT_QUADLIST:      *out_count = count * 6; return D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    default:                  *out_count = 0;          return D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
}

/* ================================================================
 * Triangle fan / quad list → triangle list conversion
 *
 * D3D11 doesn't support triangle fans or quad lists.
 * Convert vertex data in-place to triangle list.
 * Returns malloc'd buffer (caller must free) or NULL if no conversion needed.
 * ================================================================ */

static void *convert_fan_or_quad(D3DPRIMITIVETYPE pt, const void *src,
                                  UINT prim_count, UINT stride,
                                  UINT *out_vertex_count)
{
    BYTE *dst;
    const BYTE *s = (const BYTE *)src;
    UINT i;

    if (pt == D3DPT_TRIANGLEFAN) {
        /* Fan: vertex 0 is the hub, each triangle is (0, i+1, i+2) */
        UINT tri_verts = prim_count * 3;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            memcpy(dst + (i * 3 + 0) * stride, s, stride);                      /* v0 (hub) */
            memcpy(dst + (i * 3 + 1) * stride, s + (i + 1) * stride, stride);   /* v[i+1] */
            memcpy(dst + (i * 3 + 2) * stride, s + (i + 2) * stride, stride);   /* v[i+2] */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    if (pt == D3DPT_QUADLIST) {
        /* Quad list: each quad (v0,v1,v2,v3) → 2 triangles (v0,v1,v2), (v0,v2,v3) */
        UINT tri_verts = prim_count * 6;
        dst = (BYTE *)malloc(tri_verts * stride);
        if (!dst) return NULL;

        for (i = 0; i < prim_count; i++) {
            const BYTE *q = s + i * 4 * stride;
            memcpy(dst + (i * 6 + 0) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 1) * stride, q + 1 * stride, stride);  /* v1 */
            memcpy(dst + (i * 6 + 2) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 3) * stride, q + 0 * stride, stride);  /* v0 */
            memcpy(dst + (i * 6 + 4) * stride, q + 2 * stride, stride);  /* v2 */
            memcpy(dst + (i * 6 + 5) * stride, q + 3 * stride, stride);  /* v3 */
        }
        *out_vertex_count = tri_verts;
        return dst;
    }

    return NULL; /* no conversion needed */
}

/* ================================================================
 * DrawPrimitiveUP ring buffer
 *
 * Instead of creating and destroying a D3D11 buffer on every
 * DrawPrimitiveUP call, use a persistent ring buffer.
 * ================================================================ */

#define UP_RING_BUFFER_SIZE (4 * 1024 * 1024)  /* 4MB ring buffer */

static ID3D11Buffer *g_up_ring_buffer = NULL;
static UINT          g_up_ring_offset = 0;

static HRESULT up_ring_init(void)
{
    D3D11_BUFFER_DESC bd;
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = UP_RING_BUFFER_SIZE;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &g_up_ring_buffer);
}

static void up_ring_shutdown(void)
{
    if (g_up_ring_buffer) {
        ID3D11Buffer_Release(g_up_ring_buffer);
        g_up_ring_buffer = NULL;
    }
    g_up_ring_offset = 0;
}

/* Upload vertex data to ring buffer, returns offset. Returns (UINT)-1 on failure. */
static UINT up_ring_upload(const void *data, UINT size)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_MAP map_type;
    HRESULT hr;
    UINT offset;

    if (!g_up_ring_buffer) {
        if (FAILED(up_ring_init())) return (UINT)-1;
    }

    if (size > UP_RING_BUFFER_SIZE) return (UINT)-1;

    /* Wrap around if not enough space */
    if (g_up_ring_offset + size > UP_RING_BUFFER_SIZE) {
        g_up_ring_offset = 0;
        map_type = D3D11_MAP_WRITE_DISCARD;
    } else {
        map_type = D3D11_MAP_WRITE_NO_OVERWRITE;
    }

    hr = ID3D11DeviceContext_Map(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0, map_type, 0, &mapped);
    if (FAILED(hr)) return (UINT)-1;

    offset = g_up_ring_offset;
    memcpy((BYTE *)mapped.pData + offset, data, size);

    ID3D11DeviceContext_Unmap(g_device_state.d3d11_context,
        (ID3D11Resource *)g_up_ring_buffer, 0);

    g_up_ring_offset = (offset + size + 15) & ~15;  /* 16-byte align */
    return offset;
}

static HRESULT __stdcall dev_DrawPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Prepare pipeline: shaders, input layout, constant buffers, render states */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, StartVertex);
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitive(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT StartIndex, UINT PrimitiveCount)
{
    (void)self; (void)MinVertexIndex; (void)NumVertices;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT index_count;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, StartIndex, (INT)g_cur_ib_base_vertex);
    return S_OK;
}

static HRESULT __stdcall dev_DrawPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    UINT vertex_count, vb_size, ring_offset;
    const void *draw_data = pVertexData;
    void *converted = NULL;

    if (!pVertexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &vertex_count);
    if (vertex_count == 0) return E_INVALIDARG;

    /* Convert triangle fans and quad lists to triangle lists */
    if (PrimitiveType == D3DPT_TRIANGLEFAN || PrimitiveType == D3DPT_QUADLIST) {
        converted = convert_fan_or_quad(PrimitiveType, pVertexData,
                                         PrimitiveCount, VertexStreamZeroStride,
                                         &vertex_count);
        if (converted) draw_data = converted;
    }

    vb_size = vertex_count * VertexStreamZeroStride;

    /* Upload to ring buffer */
    ring_offset = up_ring_upload(draw_data, vb_size);
    if (converted) free(converted);

    if (ring_offset == (UINT)-1) return E_OUTOFMEMORY;

    /* Bind ring buffer at the right offset */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &g_up_ring_buffer, &VertexStreamZeroStride, &ring_offset);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_Draw(g_device_state.d3d11_context, vertex_count, 0);

    /* Restore previous VB binding if any */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        UINT restore_offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &restore_offset);
    }
    return S_OK;
}

static HRESULT __stdcall dev_DrawIndexedPrimitiveUP(IDirect3DDevice8 *self, D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, const void *pIndexData, D3DFORMAT IndexDataFormat, const void *pVertexData, UINT VertexStreamZeroStride)
{
    (void)self; (void)MinVertexIndex;
    g_d3d_draw_count++;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    D3D11_BUFFER_DESC bd;
    D3D11_SUBRESOURCE_DATA sd;
    ID3D11Buffer *tmp_vb = NULL, *tmp_ib = NULL;
    UINT index_count, vb_size, ib_size, offset = 0;
    UINT idx_bytes;
    DXGI_FORMAT ib_fmt;
    HRESULT hr;

    if (!pVertexData || !pIndexData || !VertexStreamZeroStride) return E_INVALIDARG;

    topology = map_primitive_type(PrimitiveType, PrimitiveCount, &index_count);
    if (index_count == 0) return E_INVALIDARG;

    idx_bytes = (IndexDataFormat == D3DFMT_INDEX32) ? 4 : 2;
    ib_fmt = (IndexDataFormat == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    vb_size = NumVertices * VertexStreamZeroStride;
    ib_size = index_count * idx_bytes;

    /* Create temp vertex buffer */
    memset(&bd, 0, sizeof(bd));
    bd.ByteWidth = vb_size;
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    memset(&sd, 0, sizeof(sd));
    sd.pSysMem = pVertexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_vb);
    if (FAILED(hr)) return hr;

    /* Create temp index buffer */
    bd.ByteWidth = ib_size;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = pIndexData;
    hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, &sd, &tmp_ib);
    if (FAILED(hr)) { ID3D11Buffer_Release(tmp_vb); return hr; }

    /* Bind, prepare, draw */
    ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
        0, 1, &tmp_vb, &VertexStreamZeroStride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
        tmp_ib, ib_fmt, 0);

    /* Vertex shader: try programmable VS first, fall back to FVF fixed-function */
    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw(); /* overrides PS if combiner shader is active */
    d3d8_states_apply();

    ID3D11DeviceContext_IASetPrimitiveTopology(g_device_state.d3d11_context, topology);
    ID3D11DeviceContext_DrawIndexed(g_device_state.d3d11_context, index_count, 0, 0);

    /* Cleanup temp buffers */
    ID3D11Buffer_Release(tmp_ib);
    ID3D11Buffer_Release(tmp_vb);

    /* Restore previous bindings */
    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(g_device_state.d3d11_context,
            0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(g_device_state.d3d11_context,
            ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

/* An indexed triangle list drawn with a vertex shader, input layout and
 * vertex constant buffers (b1, b2) the caller compiled itself -- the NV2A
 * translator's vertex programs (M4e). Everything else is what a D3D8 draw
 * sets up: the pixel shader for the current texture stages and FVF (the
 * caller's shader writes the same VS_OUT), the combiners, and the render
 * states; the caller's vertex side replaces the fixed-function one. */
/* A geometry shader for the next d3d8_DrawIndexedExternalVS calls (NULL for
 * none): with one set the indices are a point list and the shader makes the
 * primitives -- how NV2A point sprites are drawn (d3d8_extvs.c). */
static ID3D11GeometryShader *g_ext_gs;

void d3d8_SetExternalGS(ID3D11GeometryShader *gs)
{
    g_ext_gs = gs;
}

HRESULT d3d8_DrawIndexedExternalVS(const void *verts, UINT stride, UINT nverts,
                                   const uint16_t *idx, UINT nidx,
                                   ID3D11VertexShader *vs, ID3D11InputLayout *il,
                                   ID3D11Buffer *cb1, ID3D11Buffer *cb2)
{
    /* Two dynamic buffers written as rings (NO_OVERWRITE until full, then
     * DISCARD), not a pair of immutable buffers created and released per
     * batch: at several hundred batches a frame the creation was most of what
     * a busy scene cost. */
    enum { EXT_VB_BYTES = 16 * 1024 * 1024, EXT_IB_BYTES = 1024 * 1024 };
    static ID3D11Buffer *ext_vb, *ext_ib;
    static UINT vb_at, ib_at;
    D3D11_MAPPED_SUBRESOURCE map;
    ID3D11DeviceContext *ctx = g_device_state.d3d11_context;
    UINT offset = 0, vbytes, ibytes, first_index;
    HRESULT hr;

    if (!verts || !idx || !stride || !nverts || nidx < (g_ext_gs ? 1u : 3u) || !vs || !il)
        return E_INVALIDARG;
    vbytes = nverts * stride;
    ibytes = nidx * 2;
    if (vbytes > EXT_VB_BYTES || ibytes > EXT_IB_BYTES)
        return E_INVALIDARG;
    g_d3d_draw_count++;

    if (!ext_vb) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.ByteWidth = EXT_VB_BYTES;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &ext_vb);
        if (FAILED(hr)) return hr;
        bd.ByteWidth = EXT_IB_BYTES;
        bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        hr = ID3D11Device_CreateBuffer(g_device_state.d3d11_device, &bd, NULL, &ext_ib);
        if (FAILED(hr)) { ID3D11Buffer_Release(ext_vb); ext_vb = NULL; return hr; }
    }

    if (vb_at + vbytes > EXT_VB_BYTES)
        vb_at = 0;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ext_vb, 0,
                                 vb_at ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                                 0, &map);
    if (FAILED(hr)) return hr;
    memcpy((BYTE *)map.pData + vb_at, verts, vbytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ext_vb, 0);
    offset = vb_at;
    vb_at += vbytes;

    if (ib_at + ibytes > EXT_IB_BYTES)
        ib_at = 0;
    hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)ext_ib, 0,
                                 ib_at ? D3D11_MAP_WRITE_NO_OVERWRITE : D3D11_MAP_WRITE_DISCARD,
                                 0, &map);
    if (FAILED(hr)) return hr;
    memcpy((BYTE *)map.pData + ib_at, idx, ibytes);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)ext_ib, 0);
    first_index = ib_at / 2;
    ib_at += ibytes;

    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &ext_vb, &stride, &offset);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ext_ib, DXGI_FORMAT_R16_UINT, 0);

    d3d8_shaders_prepare_draw(g_device_state.vertex_shader);
    d3d8_combiners_prepare_draw();
    d3d8_states_apply();

    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_IASetInputLayout(ctx, il);
    if (cb1) ID3D11DeviceContext_VSSetConstantBuffers(ctx, 1, 1, &cb1);
    if (cb2) ID3D11DeviceContext_VSSetConstantBuffers(ctx, 2, 1, &cb2);
    if (g_ext_gs) {
        ID3D11DeviceContext_GSSetShader(ctx, g_ext_gs, NULL, 0);
        if (cb2) ID3D11DeviceContext_GSSetConstantBuffers(ctx, 2, 1, &cb2);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    } else {
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }
    ID3D11DeviceContext_DrawIndexed(ctx, nidx, first_index, 0);
    if (g_ext_gs)
        ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);

    if (g_cur_vb) {
        D3D8VertexBuffer *vb = (D3D8VertexBuffer *)g_cur_vb;
        offset = 0;
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb->d3d11_buffer, &g_cur_vb_stride, &offset);
    }
    if (g_cur_ib) {
        D3D8IndexBuffer *ib = (D3D8IndexBuffer *)g_cur_ib;
        DXGI_FORMAT fmt = (ib->format == D3DFMT_INDEX32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib->d3d11_buffer, fmt, 0);
    }
    return S_OK;
}

static HRESULT __stdcall dev_CreateTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture8 **ppTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateTextureImpl(Width, Height, Levels, Usage, Format, ppTexture);
}

static HRESULT __stdcall dev_CreateImageSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, IDirect3DSurface8 **ppSurface)
{
    (void)self;
    return d3d8_CreateImageSurfaceImpl(Width, Height, Format, ppSurface);
}

static HRESULT __stdcall dev_CreateCubeTexture(IDirect3DDevice8 *self, UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture8 **ppCubeTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateCubeTextureImpl(EdgeLength, Levels, Usage, Format, ppCubeTexture);
}

static HRESULT __stdcall dev_CreateVolumeTexture(IDirect3DDevice8 *self, UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture8 **ppVolumeTexture)
{
    (void)self; (void)Pool;
    return d3d8_CreateVolumeTextureImpl(Width, Height, Depth, Levels, Usage, Format, ppVolumeTexture);
}

static HRESULT __stdcall dev_CreateVertexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer8 **ppVertexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateVertexBufferImpl(Length, Usage, FVF, ppVertexBuffer);
}

static HRESULT __stdcall dev_CreateIndexBuffer(IDirect3DDevice8 *self, UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer8 **ppIndexBuffer)
{
    (void)self; (void)Pool;
    return d3d8_CreateIndexBufferImpl(Length, Usage, Format, ppIndexBuffer);
}

static HRESULT __stdcall dev_CreateRenderTarget(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, BOOL Lockable, IDirect3DSurface8 **ppSurface)
{
    (void)self; (void)Lockable;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    HRESULT hr;
    UINT sample_count;

    if (!ppSurface || !Width || !Height) return E_INVALIDARG;

    sample_count = d3d8_msaa_sample_count(MultiSample);
    if (sample_count > 1) {
        UINT levels = 0;
        UINT maxq = 0;
        DXGI_FORMAT fmt = d3d8_to_dxgi_format(Format);
        /* Validate against D3D11 hardware limits; fall back gracefully. */
        hr = ID3D11Device_CheckMultisampleQualityLevels(d3d8_GetD3D11Device(),
                fmt, sample_count, &maxq);
        if (FAILED(hr) || maxq == 0) {
            UINT counts[] = { 9, 8, 4, 2, 1 };
            UINT i;
            for (i = 0; i < 5; i++) {
                if (counts[i] >= sample_count) continue;
                if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(
                        d3d8_GetD3D11Device(), fmt, counts[i], &levels)) && levels > 0) {
                    sample_count = counts[i];
                    break;
                }
            }
            if (sample_count > 1) {
                fprintf(stderr, "D3D8: MSAA %lu unsupported, falling back to %lu samples\n",
                        d3d8_msaa_sample_count(MultiSample) > 1 ? (unsigned long)d3d8_msaa_sample_count(MultiSample) : 0,
                        (unsigned long)sample_count);
            } else {
                sample_count = 1;
            }
        }
    }

    memset(&td, 0, sizeof(td));
    td.Width = Width;
    td.Height = Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = d3d8_to_dxgi_format(Format);
    td.SampleDesc.Count = sample_count;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    hr = ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &td, NULL, &tex);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: CreateRenderTarget failed: 0x%08lX (fmt=0x%X %ux%u)\n", hr, Format, Width, Height);
        return hr;
    }

    *ppSurface = d3d8_surface_create(tex, 0, 0, Width, Height, Format,
                                     D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET,
                                     MultiSample, NULL, 0);
    ID3D11Texture2D_Release(tex);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_CreateDepthStencilSurface(IDirect3DDevice8 *self, UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, IDirect3DSurface8 **ppSurface)
{
    (void)self;
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D *tex = NULL;
    DXGI_FORMAT dxgi;
    D3DFORMAT surface_format = Format;
    HRESULT hr;
    UINT sample_count;

    if (!ppSurface || !Width || !Height) return E_INVALIDARG;

    dxgi = d3d8_to_dxgi_format(Format);
    if (dxgi == DXGI_FORMAT_R16_FLOAT) {
        /* F16 has no D3D11 depth equivalent; fall back to D16. */
        fprintf(stderr, "D3D8: F16 depth surface falling back to D16\n");
        dxgi = DXGI_FORMAT_D16_UNORM;
        surface_format = D3DFMT_D16;
    }

    sample_count = d3d8_msaa_sample_count(MultiSample);
    if (sample_count > 1) {
        UINT levels = 0;
        hr = ID3D11Device_CheckMultisampleQualityLevels(d3d8_GetD3D11Device(),
                dxgi, sample_count, &levels);
        if (FAILED(hr) || levels == 0) {
            UINT counts[] = { 9, 8, 4, 2, 1 };
            UINT i;
            for (i = 0; i < 5; i++) {
                if (counts[i] >= sample_count) continue;
                if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(
                        d3d8_GetD3D11Device(), dxgi, counts[i], &levels)) && levels > 0) {
                    sample_count = counts[i];
                    break;
                }
            }
            if (sample_count > 1)
                fprintf(stderr, "D3D8: depth MSAA %lu unsupported, fallback %lu samples\n",
                        (unsigned long)d3d8_msaa_sample_count(MultiSample),
                        (unsigned long)sample_count);
            else
                sample_count = 1;
        }
    }

    memset(&td, 0, sizeof(td));
    td.Width = Width;
    td.Height = Height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = dxgi;
    td.SampleDesc.Count = sample_count;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = ID3D11Device_CreateTexture2D(d3d8_GetD3D11Device(), &td, NULL, &tex);
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: CreateDepthStencilSurface failed: 0x%08lX (fmt=0x%X)\n", hr, Format);
        return hr;
    }

    *ppSurface = d3d8_surface_create(tex, 0, 0, Width, Height, surface_format,
                                     D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL,
                                     MultiSample, NULL, 0);
    ID3D11Texture2D_Release(tex);
    return *ppSurface ? S_OK : E_OUTOFMEMORY;
}

static HRESULT __stdcall dev_SetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 *pRenderTarget, IDirect3DSurface8 *pZStencilSurface)
{
    (void)self;
    ID3D11RenderTargetView *rtv;
    ID3D11DepthStencilView *dsv = NULL;

    if (g_cur_rt) { IDirect3DSurface8_Release(&g_cur_rt->iface); g_cur_rt = NULL; }
    if (g_cur_ds) { IDirect3DSurface8_Release(&g_cur_ds->iface); g_cur_ds = NULL; }

    if (pRenderTarget) {
        g_cur_rt = (D3D8Surface *)pRenderTarget;
        if (!g_cur_rt->rtv) {
            g_cur_rt = NULL;
            fprintf(stderr, "D3D8: SetRenderTarget on non-renderable surface\n");
            return E_INVALIDARG;
        }
        rtv = g_cur_rt->rtv;
        IDirect3DSurface8_AddRef(pRenderTarget);
    } else {
        rtv = g_device_state.default_rtv;
    }

    if (pZStencilSurface) {
        g_cur_ds = (D3D8Surface *)pZStencilSurface;
        if (!g_cur_ds->dsv) {
            g_cur_ds = NULL;
            fprintf(stderr, "D3D8: SetRenderTarget with non-depth stencil surface\n");
        } else {
            dsv = g_cur_ds->dsv;
            IDirect3DSurface8_AddRef(pZStencilSurface);
        }
    }

    if (!dsv) dsv = g_device_state.default_dsv;
    ID3D11DeviceContext_OMSetRenderTargets(g_device_state.d3d11_context, 1,
                                            &rtv, dsv);
    return S_OK;
}

static HRESULT __stdcall dev_GetRenderTarget(IDirect3DDevice8 *self, IDirect3DSurface8 **ppRenderTarget)
{
    (void)self;
    if (!ppRenderTarget) return E_INVALIDARG;
    if (g_cur_rt) {
        *ppRenderTarget = &g_cur_rt->iface;
        IDirect3DSurface8_AddRef(*ppRenderTarget);
    } else {
        *ppRenderTarget = NULL;
    }
    return S_OK;
}

static HRESULT __stdcall dev_GetDepthStencilSurface(IDirect3DDevice8 *self, IDirect3DSurface8 **ppZStencilSurface)
{
    (void)self;
    if (!ppZStencilSurface) return E_INVALIDARG;
    if (g_cur_ds) {
        *ppZStencilSurface = &g_cur_ds->iface;
        IDirect3DSurface8_AddRef(*ppZStencilSurface);
    } else {
        *ppZStencilSurface = NULL;
    }
    return S_OK;
}

static HRESULT __stdcall dev_SetViewport(IDirect3DDevice8 *self, const D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) {
        g_device_state.viewport = *pViewport;

        D3D11_VIEWPORT d3d11_vp;
        d3d11_vp.TopLeftX = (FLOAT)pViewport->X;
        d3d11_vp.TopLeftY = (FLOAT)pViewport->Y;
        d3d11_vp.Width    = (FLOAT)pViewport->Width;
        d3d11_vp.Height   = (FLOAT)pViewport->Height;
        d3d11_vp.MinDepth = pViewport->MinZ;
        d3d11_vp.MaxDepth = pViewport->MaxZ;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &d3d11_vp);
    }
    return S_OK;
}

/* The NV2A surface clip, as a scissor rectangle in guest pixels: a title
 * letterboxes by narrowing it. Zero width or height lifts it. The rasterizer
 * state always has the scissor test on, so a rectangle is always set. */
void d3d8_SetScissorRect(UINT x, UINT y, UINT w, UINT h)
{
    static UINT last[4] = { ~0u, ~0u, ~0u, ~0u };
    float sx = d3d8_GetCanvasScaleX(), sy = d3d8_GetCanvasScaleY();
    D3D11_RECT r;
    if (!g_device_state.d3d11_context)
        return;
    if (last[0] == x && last[1] == y && last[2] == w && last[3] == h)
        return;
    last[0] = x; last[1] = y; last[2] = w; last[3] = h;
    if (w && h) {
        r.left = (LONG)(x * sx + 0.5f);       r.top = (LONG)(y * sy + 0.5f);
        r.right = (LONG)((x + w) * sx + 0.5f); r.bottom = (LONG)((y + h) * sy + 0.5f);
    } else {
        r.left = 0; r.top = 0; r.right = 16384; r.bottom = 16384;
    }
    ID3D11DeviceContext_RSSetScissorRects(g_device_state.d3d11_context, 1, &r);
}

static HRESULT __stdcall dev_GetViewport(IDirect3DDevice8 *self, D3DVIEWPORT8 *pViewport)
{
    (void)self;
    if (pViewport) *pViewport = g_device_state.viewport;
    return S_OK;
}

static HRESULT __stdcall dev_SetMaterial(IDirect3DDevice8 *self, const D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) g_device_state.material = *pMaterial;
    return S_OK;
}

static HRESULT __stdcall dev_GetMaterial(IDirect3DDevice8 *self, D3DMATERIAL8 *pMaterial)
{
    (void)self;
    if (pMaterial) *pMaterial = g_device_state.material;
    return S_OK;
}

static HRESULT __stdcall dev_SetLight(IDirect3DDevice8 *self, DWORD Index, const D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) g_device_state.lights[Index] = *pLight;
    return S_OK;
}

static HRESULT __stdcall dev_GetLight(IDirect3DDevice8 *self, DWORD Index, D3DLIGHT8 *pLight)
{
    (void)self;
    if (Index < MAX_LIGHTS && pLight) *pLight = g_device_state.lights[Index];
    return S_OK;
}

static HRESULT __stdcall dev_LightEnable(IDirect3DDevice8 *self, DWORD Index, BOOL Enable)
{
    (void)self;
    if (Index < MAX_LIGHTS) g_device_state.light_enable[Index] = Enable;
    return S_OK;
}

static HRESULT __stdcall dev_CreateVertexShader(IDirect3DDevice8 *self, const DWORD *pDeclaration, const DWORD *pFunction, DWORD *pHandle, DWORD Usage)
{
    (void)self; (void)pDeclaration; (void)Usage;
    if (!pHandle) return E_INVALIDARG;
    if (!pFunction) return E_INVALIDARG;
    /* Count instructions: each is 4 DWORDs, last has bit 0 of word[3] set (END flag) */
    {
        int i, num_insns = 0;
        for (i = 0; i < 136; i++) {
            num_insns++;
            if (pFunction[i * 4 + 3] & 1) break;  /* END bit in last word */
        }
        return d3d8_vsh_create_shader(pFunction, num_insns, pHandle);
    }
}

static HRESULT __stdcall dev_SetVertexShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.vertex_shader = Handle;
    return S_OK;
}

static HRESULT __stdcall dev_GetVertexShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.vertex_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetVertexShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self;
    d3d8_vsh_set_constant(Register, pConstantData, ConstantCount);
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShader(IDirect3DDevice8 *self, DWORD Handle)
{
    (void)self;
    g_device_state.pixel_shader = Handle;
    d3d8_combiners_set_pixel_shader(Handle);
    return S_OK;
}

static HRESULT __stdcall dev_GetPixelShader(IDirect3DDevice8 *self, DWORD *pHandle)
{
    (void)self;
    if (pHandle) *pHandle = g_device_state.pixel_shader;
    return S_OK;
}

static HRESULT __stdcall dev_SetPixelShaderConstant(IDirect3DDevice8 *self, INT Register, const void *pConstantData, DWORD ConstantCount)
{
    (void)self; (void)Register; (void)pConstantData; (void)ConstantCount;
    return S_OK;
}

static void __stdcall dev_SetGammaRamp(IDirect3DDevice8 *self, DWORD Flags, const D3DGAMMARAMP *pRamp)
{
    (void)self; (void)Flags; /* Applied at the next presentation on both flag paths. */
    d3d8_gamma_set(pRamp);
}

static void __stdcall dev_GetGammaRamp(IDirect3DDevice8 *self, D3DGAMMARAMP *pRamp)
{
    (void)self;
    d3d8_gamma_get(pRamp);
}

static HRESULT __stdcall dev_SetPalette(IDirect3DDevice8 *self, DWORD PaletteNumber, const void *pEntries)
{
    (void)self;
    if (PaletteNumber >= D3D8_MAX_PALETTES) return E_INVALIDARG;
    d3d8_SetPalette(PaletteNumber, (const DWORD *)pEntries);

    /* A P8 texture bound to this stage bakes the palette at upload; re-bake
     * the raw indices so animated / colorized palettes take effect. */
    if (PaletteNumber < 4) {
        IDirect3DBaseTexture8 *tex = d3d8_GetStageTexture(PaletteNumber);
        if (tex) d3d8_refresh_palette(tex);
    }
    return S_OK;
}

static HRESULT __stdcall dev_BeginPush(IDirect3DDevice8 *self, DWORD Count, DWORD **ppPush)
{
    (void)self; (void)Count; (void)ppPush;
    /* TODO: Xbox push buffer emulation */
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_EndPush(IDirect3DDevice8 *self, DWORD *pPush)
{
    (void)self; (void)pPush;
    return E_NOTIMPL;
}

static HRESULT __stdcall dev_Swap(IDirect3DDevice8 *self, DWORD Flags)
{
    (void)self; (void)Flags;

    /* Pump Windows messages (same as dev_Present) */
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            ExitProcess(0);
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return d3d8_present();
}

/* ================================================================
 * Vtable
 * ================================================================ */

static const IDirect3DDevice8Vtbl g_device_vtbl = {
    dev_QueryInterface,
    dev_AddRef,
    dev_Release,
    dev_GetDirect3D,
    dev_GetDeviceCaps,
    dev_GetDisplayMode,
    dev_GetCreationParameters,
    dev_Reset,
    dev_Present,
    dev_GetBackBuffer,
    dev_BeginScene,
    dev_EndScene,
    dev_Clear,
    dev_SetTransform,
    dev_GetTransform,
    dev_SetRenderState,
    dev_GetRenderState,
    dev_SetTextureStageState,
    dev_GetTextureStageState,
    dev_SetTexture,
    dev_GetTexture,
    dev_SetStreamSource,
    dev_GetStreamSource,
    dev_SetIndices,
    dev_GetIndices,
    dev_DrawPrimitive,
    dev_DrawIndexedPrimitive,
    dev_DrawPrimitiveUP,
    dev_DrawIndexedPrimitiveUP,
    dev_CreateTexture,
    dev_CreateVertexBuffer,
    dev_CreateIndexBuffer,
    dev_CreateRenderTarget,
    dev_CreateDepthStencilSurface,
    dev_SetRenderTarget,
    dev_GetRenderTarget,
    dev_GetDepthStencilSurface,
    dev_SetViewport,
    dev_GetViewport,
    dev_SetMaterial,
    dev_GetMaterial,
    dev_SetLight,
    dev_GetLight,
    dev_LightEnable,
    dev_SetVertexShader,
    dev_GetVertexShader,
    dev_SetVertexShaderConstant,
    dev_SetPixelShader,
    dev_GetPixelShader,
    dev_SetPixelShaderConstant,
    dev_SetGammaRamp,
    dev_GetGammaRamp,
    dev_SetPalette,
    dev_BeginPush,
    dev_EndPush,
    dev_Swap,
    /* Extensions appended by xboxrecomp (slots after the historical ones) */
    dev_CreateImageSurface,
    dev_CreateCubeTexture,
    dev_CreateVolumeTexture,
};

/* ================================================================
 * Public API
 * ================================================================ */

IDirect3DDevice8 *xbox_GetD3DDevice(void)
{
    return g_device_initialized ? &g_device : NULL;
}

/* ================================================================
 * IDirect3D8 factory implementation
 * ================================================================ */

static IDirect3D8 g_d3d8;
static LONG g_d3d8_ref = 0;

static HRESULT __stdcall d3d8_QueryInterface(IDirect3D8 *self, const IID *riid, void **ppv)
{
    (void)self; (void)riid; (void)ppv;
    return E_NOINTERFACE;
}

static ULONG __stdcall d3d8_AddRef(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedIncrement(&g_d3d8_ref);
}

static ULONG __stdcall d3d8_Release(IDirect3D8 *self)
{
    (void)self;
    return (ULONG)InterlockedDecrement(&g_d3d8_ref);
}

static HRESULT __stdcall d3d8_CreateDevice(IDirect3D8 *self, UINT Adapter, DWORD DeviceType, HWND hFocusWindow, DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice8 **ppDevice)
{
    (void)self; (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
    HRESULT hr;

    if (!pPP || !ppDevice) return E_INVALIDARG;

    memset(&g_device_state, 0, sizeof(g_device_state));
    g_device_state.ref_count = 1;

    if (!pPP->hDeviceWindow) pPP->hDeviceWindow = hFocusWindow;

    hr = d3d11_create_device_and_swap_chain(&g_device_state, pPP);
    if (FAILED(hr)) return hr;

    hr = d3d11_create_render_targets(&g_device_state);
    if (FAILED(hr)) return hr;

    d3d8_init_default_states(&g_device_state);

    /* Set initial viewport (D3D11 requires explicit viewport) */
    {
        D3D11_VIEWPORT vp;
        vp.TopLeftX = 0.0f;
        vp.TopLeftY = 0.0f;
        vp.Width    = (FLOAT)g_device_state.width;
        vp.Height   = (FLOAT)g_device_state.height;
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(g_device_state.d3d11_context, 1, &vp);
    }
    d3d8_SetScissorRect(0, 0, 0, 0);

    /* Initialize shader and state subsystems */
    hr = d3d8_shaders_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Shader init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_states_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: State init failed: 0x%08lX\n", hr);
        return hr;
    }

    hr = d3d8_combiners_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: Combiner init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to fixed-function pixel shaders */
    }

    hr = d3d8_vsh_init();
    if (FAILED(hr)) {
        fprintf(stderr, "D3D8: VSH init failed: 0x%08lX\n", hr);
        /* Non-fatal: fall back to FVF vertex shaders */
    }

    g_device.lpVtbl = &g_device_vtbl;
    g_device_initialized = TRUE;

    *ppDevice = &g_device;
    fprintf(stderr, "D3D8: Device created (%ux%u, rendering at %ux%u)\n",
            g_device_state.logical_width, g_device_state.logical_height,
            g_device_state.width, g_device_state.height);
    return S_OK;
}

static const IDirect3D8Vtbl g_d3d8_vtbl = {
    d3d8_QueryInterface,
    d3d8_AddRef,
    d3d8_Release,
    d3d8_CreateDevice,
};

IDirect3D8 *xbox_Direct3DCreate8(UINT SDKVersion)
{
    (void)SDKVersion;
    g_d3d8.lpVtbl = &g_d3d8_vtbl;
    g_d3d8_ref = 1;
    return &g_d3d8;
}
