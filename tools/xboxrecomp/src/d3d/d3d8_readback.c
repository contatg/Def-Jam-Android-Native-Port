/* d3d8_readback.c -- the asynchronous back-buffer read-back of d3d8_readback.h,
 * on Direct3D 11.
 *
 * A ring of staging textures. Begin copies the picture the title drew (the
 * scaled presentation's game texture, or the swap chain's buffer 0 when the
 * picture is not scaled: what GetBackBuffer would hand out, without making a
 * surface) into a free slot with CopyResource. Poll maps the oldest slot with
 * D3D11_MAP_FLAG_DO_NOT_WAIT, which answers DXGI_ERROR_WAS_STILL_DRAWING until
 * the GPU has done the copy, and keeps it mapped until the next poll or begin.
 * Copies complete in the order they were queued, so when the oldest is not
 * done none is.
 *
 * Everything here runs on the thread that presents, against the immediate
 * context, like the rest of the device.
 */
#include "d3d8_internal.h"
#include "d3d8_readback.h"
#include <string.h>

enum { RB_FREE, RB_FLIGHT, RB_HELD };

typedef struct {
    ID3D11Texture2D *staging;
    UINT        width, height;
    DXGI_FORMAT format;
    int         state;
    uint64_t    seq, tag;
} RbSlot;

static RbSlot   g_rb[D3D8_READBACK_SLOTS];
static uint64_t g_rb_seq;

static void rb_unmap_held(void)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    int i;
    for (i = 0; i < D3D8_READBACK_SLOTS; i++) {
        if (g_rb[i].state != RB_HELD)
            continue;
        if (ctx)
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g_rb[i].staging, 0);
        g_rb[i].state = RB_FREE;
    }
}

/* 0: R, G, B, A; 1: B, G, R, A; -1: not a format the header can describe. */
static int rb_order(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return 0;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return 1;
    default:
        return -1;
    }
}

int d3d8_readback_begin(uint64_t tag)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11Texture2D *src = NULL;
    D3D11_TEXTURE2D_DESC desc;
    RbSlot *slot = NULL;
    int i, result = -1;

    if (!dev || !ctx)
        return -1;
    rb_unmap_held();
    for (i = 0; i < D3D8_READBACK_SLOTS && !slot; i++)
        if (g_rb[i].state == RB_FREE)
            slot = &g_rb[i];
    if (!slot)
        return 0;

    if (d3d8_present_scaling() && d3d8_present_game_texture()) {
        src = d3d8_present_game_texture();
        ID3D11Texture2D_AddRef(src);
    } else if (!d3d8_GetSwapChain()
               || FAILED(IDXGISwapChain_GetBuffer(d3d8_GetSwapChain(), 0, &IID_ID3D11Texture2D,
                                                  (void **)&src))) {
        return -1;
    }
    ID3D11Texture2D_GetDesc(src, &desc);
    if (desc.SampleDesc.Count > 1 || rb_order(desc.Format) < 0)
        goto done;      /* -1: LockRect resolves a multisampled target; this does not */

    if (slot->staging && (slot->width != desc.Width || slot->height != desc.Height
                          || slot->format != desc.Format)) {
        ID3D11Texture2D_Release(slot->staging);
        slot->staging = NULL;
    }
    if (!slot->staging) {
        D3D11_TEXTURE2D_DESC sd;
        memset(&sd, 0, sizeof sd);
        sd.Width = desc.Width;
        sd.Height = desc.Height;
        sd.MipLevels = 1;
        sd.ArraySize = 1;
        sd.Format = desc.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &slot->staging))) {
            slot->staging = NULL;
            goto done;
        }
        slot->width = desc.Width;
        slot->height = desc.Height;
        slot->format = desc.Format;
    }
    if (desc.MipLevels == 1 && desc.ArraySize == 1) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)slot->staging, (ID3D11Resource *)src);
    } else {
        /* The game texture has mips when the side fill blurs from them: level 0 only. */
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *)slot->staging, 0, 0, 0, 0,
                                                  (ID3D11Resource *)src, 0, NULL);
    }
    slot->state = RB_FLIGHT;
    slot->seq = ++g_rb_seq;
    slot->tag = tag;
    result = 1;
done:
    ID3D11Texture2D_Release(src);
    return result;
}

int d3d8_readback_poll(D3D8Readback *out)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    D3D11_MAPPED_SUBRESOURCE mapped;
    RbSlot *oldest = NULL;
    int i;

    if (!ctx)
        return 0;
    rb_unmap_held();
    for (i = 0; i < D3D8_READBACK_SLOTS; i++)
        if (g_rb[i].state == RB_FLIGHT && (!oldest || g_rb[i].seq < oldest->seq))
            oldest = &g_rb[i];
    if (!oldest || !out)
        return 0;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)oldest->staging, 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        return 0;       /* DXGI_ERROR_WAS_STILL_DRAWING, or the device is gone */
    oldest->state = RB_HELD;
    out->pixels = (const uint8_t *)mapped.pData;
    out->pitch = (int)mapped.RowPitch;
    out->width = oldest->width;
    out->height = oldest->height;
    out->bgra = rb_order(oldest->format);
    out->tag = oldest->tag;
    return 1;
}

void d3d8_readback_reset(void)
{
    int i;
    rb_unmap_held();
    for (i = 0; i < D3D8_READBACK_SLOTS; i++) {
        if (g_rb[i].staging)
            ID3D11Texture2D_Release(g_rb[i].staging);
        memset(&g_rb[i], 0, sizeof g_rb[i]);
    }
}
