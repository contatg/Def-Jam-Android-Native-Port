/*
 * d3d8_vk.h -- what the Vulkan backend needs from the program that hosts it.
 *
 * The backend creates the Vulkan instance and device and draws; it does not
 * create a window. A host that has one says which instance extensions its
 * window system needs and how to make a surface for it, before the device is
 * created. With no host set (or a host that returns no surface) the backend
 * renders off screen: every frame is drawn and can be captured, and nothing
 * is presented. That is what an unattended test run on a machine without a
 * display uses.
 */
#ifndef XBOXRECOMP_D3D8_VK_H
#define XBOXRECOMP_D3D8_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D8VkHost {
    /* Instance extensions the window system needs (VK_KHR_surface and its
     * platform one), as SDL_Vulkan_GetInstanceExtensions returns them. */
    const char *const *instance_extensions;
    uint32_t           instance_extension_count;
    /* Make a VkSurfaceKHR for the window: instance is a VkInstance, surface
     * points at a VkSurfaceKHR. Returns non-zero on success. Called once,
     * from the thread that creates the device. */
    int  (*create_surface)(void *instance, void *surface, void *user);
    /* The window's size in pixels, asked whenever the swap chain is built. */
    void (*drawable_size)(int *width, int *height, void *user);
    void *user;
    /* The refresh rate of the display the window is on, in whole Hz (0 when
     * unknown), asked whenever the swap chain is built. Optional. */
    unsigned (*refresh_hz)(void *user);
} D3D8VkHost;

void d3d8_vk_set_host(const D3D8VkHost *host);

#if defined(__ANDROID__)
/* Creates an Android Vulkan surface using the loader selected by the renderer
 * (system or custom Turnip). native_window is ANativeWindow*. */
int d3d8_vk_android_create_surface(void *instance, void *native_window, void *surface);
#endif

/* A host program's drawing on top of the picture, in window pixels, after the
 * title's target has been scaled into the window: a menu, an on-screen display.
 * The Vulkan counterpart of d3d8_present_set_overlay.
 *
 * Each frame the backend asks `active`; when it says yes, the backend begins a
 * render pass on the window's image (colour attachment, the picture already in
 * it, kept), calls `draw` on the thread that presents with the frame's command
 * buffer recording inside that pass, and ends it. `draw` records draw commands
 * and nothing else it must not submit that buffer. Handles are Vulkan's, as
 * plain pointers so this header does not need vulkan.h.
 *
 * `generation` changes whenever `render_pass` or the image count was replaced
 * (a swap chain with another format); a host that built pipelines against the
 * pass makes them again. The picture a harness captures is the title's target,
 * so nothing drawn here is ever in a capture. */
typedef struct D3D8VkOverlayFrame {
    void    *instance;          /* VkInstance */
    void    *physical_device;   /* VkPhysicalDevice */
    void    *device;            /* VkDevice */
    void    *queue;             /* VkQueue, the one that submits the frame */
    uint32_t queue_family;
    void    *command_buffer;    /* VkCommandBuffer, inside the pass */
    void    *render_pass;       /* VkRenderPass the draw is compatible with */
    uint32_t image_count;       /* swap chain images */
    uint32_t width, height;     /* window pixels */
    int      format;            /* VkFormat of the window's image */
    uint32_t generation;
} D3D8VkOverlayFrame;

typedef struct D3D8VkOverlay {
    int  (*active)(void *user);
    void (*draw)(const D3D8VkOverlayFrame *frame, void *user);
    void *user;
} D3D8VkOverlay;

/* NULL removes it. Safe to call before the device exists. */
void d3d8_vk_set_overlay(const D3D8VkOverlay *overlay);

/* A folder to keep compiled shaders and the driver's pipeline cache in between
 * runs (d3d8_vk_cache.h), made if missing. Without one, or with NULL or "",
 * every run compiles its shaders afresh, as before. Call before the device is
 * created. RECOMP_SHADER_CACHE, when set, overrides it: a folder, or 0 / off
 * for none. */
void d3d8_vk_set_cache_dir(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VK_H */
