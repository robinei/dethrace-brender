#ifndef _DEVICE_P_H_
#define _DEVICE_P_H_

/*
    This file was added by dethrace.
    Nasty hack to provide device-specific types to external consumers of BRender
*/

#include "compiler.h"
#include "pixelmap.h"

// For use with `virtualframebuffer` device.
typedef void br_device_virtualfb_swapbuffers_cbfn(br_pixelmap* pm);
typedef void br_device_virtualfb_palette_changed_cbfn(br_uint_32* palette_entries);

typedef struct br_device_virtualfb_callback_procs {
    br_device_virtualfb_palette_changed_cbfn      *palette_changed;
    br_device_virtualfb_swapbuffers_cbfn          *swap_buffers;
} br_device_virtualfb_callback_procs;

// For use with `glrend` device.
typedef void BR_CALLBACK br_device_gl_swapbuffers_cbfn(br_pixelmap* pm);
typedef void* BR_CALLBACK br_device_gl_getprocaddress_cbfn(const char* name);
typedef void BR_CALLBACK br_device_gl_free_cbfn(br_pixelmap* pm, void* user);
typedef void BR_CALLBACK br_device_gl_getviewport_cbfn(int *x, int *y, float *width_multiplier, float *height_multiplier);
// Optional. Called before rendering a scene into `target` with the viewport (in window pixels, bottom-left origin)
// that will be used. The callback may modify the viewport, eg. to extend the 3d scene to fill a widescreen window.
typedef void BR_CALLBACK br_device_gl_adjustrenderviewport_cbfn(br_pixelmap* target, int *x, int *y, int *width, int *height);
// Optional. Regions of the 2d overlay to draw moved horizontally, eg. to put HUD elements at the edges of a
// widescreen window. Sets `*regions` to `count` groups of 5 floats: left, top, right, bottom (overlay pixels,
// exclusive right/bottom) and the horizontal shift (overlay pixels, may move them outside the overlay's 4:3 area).
// The moved areas are not drawn at their original position. Returns the region count.
#define BR_GL_MAX_OVERLAY_REGIONS 48
typedef int BR_CALLBACK br_device_gl_overlayregions_cbfn(const float **regions);

typedef struct br_device_gl_callback_procs {
    br_device_gl_getprocaddress_cbfn      *get_proc_address;
    br_device_gl_getviewport_cbfn         *get_viewport;
    br_device_gl_swapbuffers_cbfn         *swap_buffers;
    br_device_gl_free_cbfn                *free;
    br_device_gl_adjustrenderviewport_cbfn *adjust_render_viewport;
    br_int_32                              texture_sharp;   /* Non-zero: unfiltered textures get anti-aliased texel edges */
    br_int_32                              texture_upscaler; /* 2x magnification of paletted textures: 0 = none, 1 = MMPX, 2 = xBR */
    br_device_gl_overlayregions_cbfn       *overlay_regions;
} br_device_gl_callback_procs;

#endif
