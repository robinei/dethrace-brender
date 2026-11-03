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
#define BR_GL_MAX_OVERLAY_REGIONS 64
typedef int BR_CALLBACK br_device_gl_overlayregions_cbfn(const float **regions);
// Optional. Queried for each draw: when above 0, geometry fades out (to whatever was drawn behind it, eg. the
// sky) between this fraction of the yon distance and the yon distance itself
typedef float BR_CALLBACK br_device_gl_distancefade_cbfn(void);
// Optional. Light sources of a paletted texture, by its pixelmap identifier: sets `indices` to a 256 bit mask of
// the palette indices that glow and, if `rect_count` is set non-zero, `rects` to x, y, width, height of the
// areas they are limited to. Returns the glow intensity, 0 for none.
// Optional. Queried at the start of each scene: sets `*brightness` (1 = normal, below 1 darkens the scene except
// for light source texels, see texture_glow) and `*lights` to `count` lights of 12 floats each: view space
// position, range, view space direction (normalised; zero for a point light), strength, colour (rgb) and haze
// (how much the light shows in the air, eg. headlight beams in fog). Lit surfaces get at most their normal
// brightness. Returns the light count.
#define BR_GL_MAX_SCENE_LIGHTS 32
#define BR_GL_SCENE_LIGHT_FLOATS 12
typedef int BR_CALLBACK br_device_gl_scenelighting_cbfn(float *brightness, const float **lights);
typedef float BR_CALLBACK br_device_gl_textureglow_cbfn(const char *identifier, const unsigned char **indices, const short **rects, int *rect_count, int *id);
// Optional. Queried when drawing with a glowing texture: scales its glow, by the `id` texture_glow gave it
// (eg. brighter brake lights while braking)
typedef float BR_CALLBACK br_device_gl_glowscale_cbfn(int id);
// Optional. Queried at the start of each scene, for the textured surfaces with normals: sets 16 floats,
// the sun's direction (towards it) in view space and how much surfaces facing away from it darken (0 = none),
// then the world's up in view space and how much a large scale brightness variation breaks up the tiled
// textures (0 = none); then a haze colour (rgb) and how much (at most) of it everything takes on with distance,
// and the distance by which most of that is reached (aerial perspective). Returns non-zero to use them. Also
// queried for each draw, so it can change within a scene
#define BR_GL_SCENE_SURFACE_FLOATS 16
typedef int BR_CALLBACK br_device_gl_scenesurface_cbfn(float *params);
// Optional. Queried for each draw: zero to leave the depth buffer alone (eg. for the sky, so the effects
// reading depth take it for infinitely far)
typedef int BR_CALLBACK br_device_gl_depthwrite_cbfn(void);

typedef struct br_device_gl_callback_procs {
    br_device_gl_getprocaddress_cbfn      *get_proc_address;
    br_device_gl_getviewport_cbfn         *get_viewport;
    br_device_gl_swapbuffers_cbfn         *swap_buffers;
    br_device_gl_free_cbfn                *free;
    br_device_gl_adjustrenderviewport_cbfn *adjust_render_viewport;
    br_int_32                              texture_sharp;   /* Non-zero: unfiltered textures get anti-aliased texel edges */
    br_int_32                              texture_upscaler; /* 2x magnification of paletted textures: 0 = none, 1 = MMPX, 2 = xBR */
    br_device_gl_overlayregions_cbfn       *overlay_regions;
    br_device_gl_distancefade_cbfn         *distance_fade;
    br_device_gl_textureglow_cbfn          *texture_glow;
    br_device_gl_glowscale_cbfn            *glow_scale;
    br_device_gl_scenelighting_cbfn        *scene_lighting;
    br_device_gl_scenesurface_cbfn         *scene_surface;
    br_device_gl_depthwrite_cbfn           *depth_write;
    br_int_32                              fog_distance;    /* Non-zero: fog by distance, from fog_min to fully fogged at fog_max */
} br_device_gl_callback_procs;

#endif
