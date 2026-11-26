/*
 * Stored buffer structure
 */
#ifndef _SBUFFER_H_
#define _SBUFFER_H_

#ifdef __cplusplus
extern "C" {
#endif

#ifdef BR_BUFFER_STORED_PRIVATE

/*
 * Block structure
 */
typedef struct br_buffer_stored {
    /*
     * Dispatch table
     */
    struct br_buffer_stored_dispatch *dispatch;

    /*
     * Standard object identifier
     */
    const char *identifier;

    br_device   *device;
    br_renderer *renderer;

    /*
     * Source pixelmap
     */
    br_pixelmap *source;

    /*
     * Copy of source flags
     */
    br_uint_16 source_flags;

    /*
     * OpenGL texture handle
     */
    GLuint gl_tex;

    GLenum gl_internal_format;
    GLenum gl_format;
    GLenum gl_type;

    br_boolean blended;

    /*
     * Object query templates.
     * FIXME: These should be stored on the device, but that can't
     *  be done until everything's ported to C.
     */
    struct br_tv_template *templates;

    /*
     * Glow mask (light sources) of a paletted texture, from the device's texture_glow callback
     */
    GLuint glow_tex;
    float glow_intensity;
    int glow_id;
    br_boolean glow_checked;

    /*
     * How much surfaces with this texture mirror their surroundings, from the device's texture_reflect callback
     */
    float reflect;
    br_boolean reflect_checked;
    int wear_slot; /* see texture_slot, -1 for none */

    br_boolean opaque; /* has no colour key (black) texels, so nothing is cut out of it (FALSE until known) */
    GLint gl_min_filter, gl_mag_filter; /* the filtering last set on it (0 for not yet) */
    GLfloat gl_anisotropy;

    br_boolean paletted_source_dirty;
    br_boolean converted; /* (converted at least once) */
    int drawnFrame; /* the frame it was last drawn in (see VIDEOI_UpdatePalettedTexture) */
    br_uint_32 palette_revision;
    struct br_device_clut *palette_pointer;

} br_buffer_stored;

#endif

#ifdef __cplusplus
};
#endif
#endif
