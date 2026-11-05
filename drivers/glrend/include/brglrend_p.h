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
    float wet;                  /* reflections of the glowing lights on the ground (needs glow), 0 disables */
    float up[3];                /* the world's up, in view space */
    float ice;                  /* mirror images on ice (see texture_reflect, needs glow), 0 disables */
    float water;                /* rippled mirror images on water (texture_reflect 0.5, needs glow), 0 disables */
    float to_world[12];         /* view to world space, as a br_matrix34 (rows: x, y and z axes, position) */
    float paint;                /* glossy car paint (texture_reflect 0.25, needs glow), 0 disables */
    float weather[3];           /* snow or rain: 1 for rain (else snow), the amount (0 for none), their brightness */
    float sun[4];               /* sun shadows: the sun's direction in view space, and their strength (0 disables) */
    float shadow_view[3 * 12];  /* world to the shadow maps' cameras, as br_matrix34s (used if a map was drawn) */
    float sun_shading;          /* the strength of the scenes' sun shading (scene_surface), which the shadows match */
    float specular;             /* the sun's highlights (in the direction of sun), 0 disables */
    int shimmer_count;          /* heat shimmer above these points: view space position and strength, up to 8 */
    float shimmers[4 * 8];
    int lamp_count;
    float lamps[8 * 24]; /* lamps: view space position and size (the reach of their glow), colour and strength */
    int spray_count;
    float spray[8 * 24]; /* wheels throwing up spray: world position and strength, the car's velocity and a seed */
} br_gl_post_params;

br_error BR_PUBLIC_ENTRY BrGLPostProcessBegin(br_pixelmap *screen, br_boolean glow);
void BR_PUBLIC_ENTRY BrGLPostProcessEnd(br_pixelmap *screen, const br_gl_post_params *params);
/* The sun's shadow maps (up to 3 levels, cascaded: each covers more than the one before): the scenes drawn
 * between Begin and End (with a parallel camera at the sun) only write the depth of that level's map, size x
 * size; translucent draws are left out. The post-processing then shades by them (see
 * br_gl_post_params.shadow_view), the first level that covers a point taking precedence */
br_error BR_PUBLIC_ENTRY BrGLShadowPassBegin(br_pixelmap *screen, int size, int level);
/* What is drawn while on is sprites (flat cards facing the camera), which the sun shadows treat as such */
void BR_PUBLIC_ENTRY BrGLSprites(br_pixelmap *screen, int on);
void BR_PUBLIC_ENTRY BrGLShadowPassEnd(br_pixelmap *screen);
/* While post-processing, after a depth snapshot: blended draws fade out within `distance` (view units) in front
 * of the snapshot's depth, so sprites don't show hard lines where they cut into it; 0 turns it off */
void BR_PUBLIC_ENTRY BrGLPostProcessSoftParticles(br_pixelmap *screen, float distance);
/* While post-processing: take the depth drawn so far for the ambient occlusion, so what is drawn after it
 * (sprites, smoke, sparks) neither casts nor receives it. Without it, the final depth is used */
void BR_PUBLIC_ENTRY BrGLPostProcessDepthSnapshot(br_pixelmap *screen);
/* Draw into a second overlay layer of the (locked) back screen, which is drawn moved by the overlay regions'
 * shifts (eg. the HUD elements of a widescreen window), or back into the first */
void BR_PUBLIC_ENTRY BrGLOverlaySideLayer(br_pixelmap *pm, br_boolean on);

#endif /* _NO_PROTOTYPES */

#ifdef __cplusplus
};
#endif
#endif
