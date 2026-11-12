/*
 * d3d8_formats.c -- facts about Xbox D3D8 formats that do not depend on the
 * graphics backend, so every backend and the push-buffer executor share them.
 */
#include "d3d8_xbox.h"

UINT d3d8_format_bpp(D3DFORMAT fmt);

UINT d3d8_format_bpp(D3DFORMAT fmt)
{
    switch (fmt) {
    /* 32 bits per pixel */
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_LIN_A8R8G8B8:
    case D3DFMT_LIN_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_LIN_A8B8G8R8:
    case D3DFMT_B8G8R8A8:
    case D3DFMT_LIN_B8G8R8A8:
    case D3DFMT_R8G8B8A8:
    case D3DFMT_LIN_R8G8B8A8:
    case D3DFMT_Q8W8V8U8:
    case D3DFMT_X8L8V8U8:
    case D3DFMT_V16U16:
    case D3DFMT_LIN_V16U16:
    case D3DFMT_D24S8:
    case D3DFMT_F24S8:
    case D3DFMT_LIN_D24S8:
    case D3DFMT_LIN_F24S8:
    case D3DFMT_D24X8:
    case D3DFMT_D24FS8:
    case D3DFMT_D32:
    case D3DFMT_LIN_D24X8:
    case D3DFMT_LIN_D24FS8:
    case D3DFMT_LIN_D32:
    case D3DFMT_G16R16:
    case D3DFMT_LIN_G16R16:
    case D3DFMT_A16L16:
    case D3DFMT_LIN_A16L16:
    case D3DFMT_L32:
    case D3DFMT_LIN_L32:
    case D3DFMT_R32F:
    case D3DFMT_LIN_R32F:
    case D3DFMT_G16R16F:
    case D3DFMT_LIN_G16R16F:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_X2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_A2W10V10U10:
    case D3DFMT_R10G11B11:
    case D3DFMT_R11G11B10:
    case D3DFMT_LIN_A2R10G10B10:
    case D3DFMT_LIN_X2R10G10B10:
    case D3DFMT_LIN_A2B10G10R10:
    case D3DFMT_LIN_A2W10V10U10:
    case D3DFMT_LIN_R10G11B11:
    case D3DFMT_LIN_R11G11B10:
    case D3DFMT_INDEX32:
        return 32;

    /* 16 bits per pixel */
    case D3DFMT_A1R5G5B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_LIN_A1R5G5B5:
    case D3DFMT_LIN_X1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_LIN_A4R4G4B4:
    case D3DFMT_R5G6B5:
    case D3DFMT_LIN_R5G6B5:
    case D3DFMT_R6G5B5:
    case D3DFMT_LIN_R6G5B5:
    case D3DFMT_V8U8:
    case D3DFMT_LIN_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_LIN_L6V5U5:
    case D3DFMT_G8B8:
    case D3DFMT_LIN_G8B8:
    case D3DFMT_R8B8:
    case D3DFMT_LIN_R8B8:
    case D3DFMT_A8L8:
    case D3DFMT_LIN_A8L8:
    case D3DFMT_D16:
    case D3DFMT_LIN_D16:
    case D3DFMT_F16:
    case D3DFMT_LIN_F16:
    case D3DFMT_L16:
    case D3DFMT_LIN_L16:
    case D3DFMT_R5G5B5A1:
    case D3DFMT_LIN_R5G5B5A1:
    case D3DFMT_R4G4B4A4:
    case D3DFMT_LIN_R4G4B4A4:
    case D3DFMT_R16F:
    case D3DFMT_LIN_R16F:
    case D3DFMT_YUY2:
    case D3DFMT_UYVY:
    case D3DFMT_INDEX16:
        return 16;

    /* 8 bits per pixel */
    case D3DFMT_L8:
    case D3DFMT_LIN_L8:
    case D3DFMT_AL8:
    case D3DFMT_LIN_AL8:
    case D3DFMT_A8:
    case D3DFMT_LIN_A8:
    case D3DFMT_P8:
        return 8;

    /* 64 bits per pixel */
    case D3DFMT_A16B16G16R16:
    case D3DFMT_LIN_A16B16G16R16:
    case D3DFMT_G32R32:
    case D3DFMT_LIN_G32R32:
    case D3DFMT_A32L32:
    case D3DFMT_LIN_A32L32:
    case D3DFMT_G32R32F:
    case D3DFMT_LIN_G32R32F:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_LIN_A16B16G16R16F:
    case D3DFMT_V32U32:
    case D3DFMT_LIN_V32U32:
    case D3DFMT_Q16W16V16U16:
    case D3DFMT_LIN_Q16W16V16U16:
        return 64;

    /* 128 bits per pixel */
    case D3DFMT_A32B32G32R32:
    case D3DFMT_LIN_A32B32G32R32:
    case D3DFMT_A32B32G32R32F:
    case D3DFMT_LIN_A32B32G32R32F:
    case D3DFMT_Q32W32V32U32:
    case D3DFMT_LIN_Q32W32V32U32:
        return 128;

    /* Compressed (bits per pixel of the source data) */
    case D3DFMT_DXT1:
    case D3DFMT_CTX1:
    case D3DFMT_LIN_CTX1:
        return 4;   /* BC1 */
    case D3DFMT_DXT3:
    case D3DFMT_DXT3A:
    case D3DFMT_DXT5:
    case D3DFMT_DXT5A:
    case D3DFMT_DXN:
    case D3DFMT_LIN_DXT3A:
    case D3DFMT_LIN_DXT5A:
    case D3DFMT_LIN_DXN:
        return 8;   /* BC2/BC3/BC5 */
    default: return 32;
    }
}
