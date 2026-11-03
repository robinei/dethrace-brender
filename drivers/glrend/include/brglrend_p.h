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
    float flare_strength;       /* light streaks from the glow */
    float ao_strength;          /* ambient occlusion, 0 disables */
    float ao_radius;            /* in view units */
    float speed;                /* speed blur, 0..1 */
    float grade;                /* colour grading strength, 0 disables */
    float night;                /* 1 at night (cooler grading) */
    float time;                 /* seconds, for animated effects */
    int shimmer_count;          /* heat shimmer above these points: view space position and strength, up to 8 */
    float shimmers[4 * 8];
} br_gl_post_params;

br_error BR_PUBLIC_ENTRY BrGLPostProcessBegin(br_pixelmap *screen, br_boolean glow);
void BR_PUBLIC_ENTRY BrGLPostProcessEnd(br_pixelmap *screen, const br_gl_post_params *params);
/* While post-processing: take the depth drawn so far for the ambient occlusion, so what is drawn after it
 * (sprites, smoke, sparks) neither casts nor receives it. Without it, the final depth is used */
void BR_PUBLIC_ENTRY BrGLPostProcessDepthSnapshot(br_pixelmap *screen);

#endif /* _NO_PROTOTYPES */

#ifdef __cplusplus
};
#endif
#endif
