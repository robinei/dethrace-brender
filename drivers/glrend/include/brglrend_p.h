#ifndef _BRGLREND_P_H
#define _BRGLREND_P_H

#ifdef __cplusplus
extern "C" {
#endif

#ifndef _NO_PROTOTYPES

/*
 * Main entry point for device.
 */
struct br_device *BR_EXPORT BrDrv1GLBegin(const char *arguments);

/*
 * Post-processing of a view: rendering between Begin and End goes to an offscreen target, which End
 * then draws to the screen with the effects applied. `screen` is the front screen pixelmap.
 */
typedef struct br_gl_post_params {
    float hither, yon;          /* depth range of the view's camera */
    float dof_start, dof_end;   /* depth of field: distances where the blur starts and is at its strongest */
    float dof_max_radius;       /* strongest blur radius, in window pixels; 0 disables depth of field */
    float glow_strength;        /* bloom around light sources (see texture_glow), if Begin was asked for it */
} br_gl_post_params;

br_error BR_PUBLIC_ENTRY BrGLPostProcessBegin(br_pixelmap *screen, br_boolean glow);
void BR_PUBLIC_ENTRY BrGLPostProcessEnd(br_pixelmap *screen, const br_gl_post_params *params);

#endif /* _NO_PROTOTYPES */

#ifdef __cplusplus
};
#endif
#endif
