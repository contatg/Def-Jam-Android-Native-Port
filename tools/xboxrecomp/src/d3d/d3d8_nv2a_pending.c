/*
 * d3d8_nv2a_pending.c -- the push-buffer executor's entry points that only
 * the Direct3D 11 backend implements so far.
 *
 * Off Windows these exist so the runtime links and its hardware-free tests
 * run. Nothing is drawn through them: a title that renders through the
 * executor shows nothing on these hosts until the second backend provides
 * them, and the first call says so.
 */
#include "d3d8_xbox.h"
#include <stdint.h>
#include <stdio.h>

static void pending(const char *what)
{
    static int said;
    if (!said) {
        said = 1;
        fprintf(stderr, "D3D8: %s is not implemented by this graphics backend;"
                        " push-buffer rendering is incomplete on this host\n", what);
    }
}

void d3d8_SetScissorRect(UINT x, UINT y, UINT w, UINT h)
{ (void)x; (void)y; (void)w; (void)h; pending("d3d8_SetScissorRect"); }

void d3d8_SetBlendColor(DWORD argb)
{ (void)argb; pending("d3d8_SetBlendColor"); }

void d3d8_Nv2aPointMode(int on, float fixed_size)
{ (void)on; (void)fixed_size; pending("d3d8_Nv2aPointMode"); }

/* -1: this program cannot be drawn here, which is the documented answer. */
int d3d8_Nv2aProgramDraw(uint32_t hash, const char *hlsl, uint32_t used,
                         const float *consts, uint32_t consts_version,
                         float zscale, const float texscale[2],
                         const float *verts, uint32_t nverts,
                         const uint16_t *idx, uint32_t nidx)
{
    (void)hash; (void)hlsl; (void)used; (void)consts; (void)consts_version;
    (void)zscale; (void)texscale; (void)verts; (void)nverts; (void)idx; (void)nidx;
    pending("d3d8_Nv2aProgramDraw");
    return -1;
}

void d3d8_combiners_set_nv2a(const DWORD color_icw[8], const DWORD alpha_icw[8],
                             const DWORD color_ocw[8], const DWORD alpha_ocw[8],
                             const DWORD factor0[8], const DWORD factor1[8],
                             DWORD control, DWORD final_cw0, DWORD final_cw1,
                             DWORD final_c0, DWORD final_c1, DWORD shader_stages)
{
    (void)color_icw; (void)alpha_icw; (void)color_ocw; (void)alpha_ocw;
    (void)factor0; (void)factor1; (void)control; (void)final_cw0; (void)final_cw1;
    (void)final_c0; (void)final_c1; (void)shader_stages;
    pending("d3d8_combiners_set_nv2a");
}

void d3d8_combiners_clear_nv2a(void) { pending("d3d8_combiners_clear_nv2a"); }

/* Presentation settings and the gamma ramp: accepted and not applied, because
 * there is no presenter here to apply them to yet. */
void d3d8_present_enable_scaling(int enable)            { (void)enable; }
void d3d8_present_set_render_scale(unsigned scale)      { (void)scale; }
void d3d8_present_set_aspect(int keep, unsigned num, unsigned den)
{ (void)keep; (void)num; (void)den; }
void d3d8_present_set_vsync(int vsync)                  { (void)vsync; }
void d3d8_present_set_linear_filter(int linear)         { (void)linear; }
void d3d8_gamma_set(const void *ramp)                   { (void)ramp; }
