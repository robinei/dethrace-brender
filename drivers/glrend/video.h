/*
 * VIDEO structures
 */
#ifndef VIDEO_H_
#define VIDEO_H_

#ifdef __cplusplus
extern "C" {
#endif


#define BR_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)

/* The uniforms of brender.frag/vert: their locations in a program (-1 where it has none) */
typedef struct brender_uniforms {
    GLint main_texture;      /* sampler2D */
    GLint sample_coverage;   /* bool */
    GLint sharp_filter;      /* bool */
    GLint fog_distance;      /* bool */
    GLint glow_texture;      /* sampler2D */
    GLint glow_intensity;    /* float */
    GLint surface_reflect;   /* float */
    GLint soft_depth;        /* sampler2D */
    GLint soft_distance;     /* float */
    GLint surface_wear;      /* float */
    GLint surface_sprite;    /* bool */
    GLint surface_soft;      /* bool */
    GLint scene_brightness;  /* float */
    GLint num_spots;         /* int */
    GLint spots;             /* vec4[3 * MAX] */
    GLint num_draw_spots;    /* int */
    GLint draw_spots;        /* int[MAX] */
    GLint surface_sun;       /* vec4 */
    GLint surface_up;        /* vec4 */
    GLint surface_haze;      /* vec4[2] */
} brender_uniforms;

enum { BRENDER_FULL, BRENDER_OPAQUE, BRENDER_SHADOW, BRENDER_SHADOW_OPAQUE, BRENDER_VARIANTS };

/* brender.frag's features (its F_*), a bit each, in its order: what a draw needs (see VIDEOI_UseBRenderFeatures) */
enum {
    BRF_NIGHT = 1 << 0,
    BRF_SHADING = 1 << 1,
    BRF_HAZE = 1 << 2,
    BRF_FOG = 1 << 3,
    BRF_ENV = 1 << 4,
    BRF_SHARP = 1 << 5,
    BRF_SOFT = 1 << 6,
    BRF_SOFT_DEPTH = 1 << 7,
    BRF_GLOW = 1 << 8,
    BRF_WEAR = 1 << 9,
    BRF_POST = 1 << 10,
    BRF_LIGHTS = 1 << 11,
    BRF_BLOBS = 1 << 12,
    BRF_LIGHT_HAZE = 1 << 13,
    BRF_COUNT = 14
};
/* the most programs built for features (beyond that the general ones draw) */
#define BRENDER_FEATURE_PROGRAMS 64

/* brender.vert/frag as built for some of what is drawn, and the uniforms last given to it */
typedef struct brender_variant {
    GLuint program;
    brender_uniforms uniforms;
    int sceneSerial; /* the scene whose uniforms it has (see scene_uniforms) */
    float surfaceUploaded[BR_GL_SCENE_SURFACE_FLOATS]; /* the scene_surface parameters last given to it */
    int drawSpots[BR_GL_MAX_SCENE_LIGHTS], drawSpotCount; /* the draw_spots last given to it */
    /* the per draw uniforms last given to it (see apply_stored_properties; reset with the scene's uniforms) */
    float glowIntensity, reflect, wear, softDistance;
    int sharp, sprite, soft, sampleCoverage;
    /* (built for features) BRENDER_FULL or BRENDER_OPAQUE, the features; whether it is in the program list (see
     * VIDEOI_UseBRenderFeatures) */
    int base;
    unsigned features;
    int listed, failed;
} brender_variant;

/* post.frag's passes (its stages): 0 all at once, 1 the lit scene, 2 the blurs and the rest of it, 3 the sun's
 * shadow at half resolution, 4 the wet ground's reflections at half resolution, 5 the depth of field at half
 * resolution, 6 the bloom and lens flare at half resolution */
#define POST_STAGES 7

typedef struct _VIDEO {
    GLint maxUniformBlockSize;
    GLint maxUniformBufferBindings;
    GLint maxVertexUniformBlocks;
    GLint maxFragmentUniformBlocks;
    GLint maxSamples;
    GLfloat maxAnisotropy;
    br_int_32 textureSharp; /* Sample unfiltered textures with anti-aliased texel edges */
    br_int_32 textureUpscaler; /* 2x magnification of paletted textures: 0 = none, 1 = MMPX, 2 = xBR */
    br_int_32 fogDistance;     /* Fog by distance, from fog_min to fully fogged at fog_max */
    br_int_32 msaaSamples;     /* The post-processed scene's multisampling (callbacks' msaa_samples) */
    br_device_gl_textureglow_cbfn* textureGlow;
    br_device_gl_glowscale_cbfn* glowScale;
    br_device_gl_scenelighting_cbfn* sceneLighting;
    br_device_gl_scenesurface_cbfn* sceneSurface;
    br_device_gl_depthwrite_cbfn* depthWrite;
    br_device_gl_texturereflect_cbfn* textureReflect;
    br_device_gl_textureslot_cbfn* textureSlot;
    br_device_gl_slotwear_cbfn* slotWear;
    char programList[512]; /* (callbacks' program_list, "" for none) */

    struct {
        GLuint program;
        GLint aPosition;     /* Position, vec3 */
        GLint aColour;       /* Colour, vec3 */
        GLint aUV;           /* UV, vec2 */
        GLint uSampler;      /* Sampler, sampler2D */
        GLint uMVP;          /* Model-View-Projection Matrix, mat4 */
        GLint uFlipVertically;
        GLint uDiscardPurplePixels;
        GLint uSharp;
        GLint uLayerWidth;
        GLint uLayerMargin;
        GLint uRegionCount;
        GLint uRegions;
        GLint uRegionShifts;
        GLint uRegionGrid;
        GLuint regionGridTex; /* the regions near each 8x8 cell of the layer, see update_region_grid */
    } defaultProgram;

    struct {
        GLuint program;

        struct {
            GLint aPosition; /* Vectex Position, vec3 */
            GLint aUV;       /* UV, vec2 */
            GLint aNormal;   /* Vertex Normal, vec3 */
            GLint aColour;   /* Vertex colour, vec4 */
        } attributes;

        GLuint uboScene;
        GLuint blockIndexScene;
        GLuint blockBindingScene;

        GLuint uboModel;
        /* (a ring: each draw's model state at the next free place in it, so the buffer isn't made anew per draw
         * (see StoredGLRenderGroup); from the start again, in a fresh buffer, when it is full) */
        GLsizeiptr uboModelSize, uboModelUsed;
        GLint uboModelAlign;
        GLuint blockIndexModel;
        GLuint blockBindingModel;

        GLint mainTextureBinding;
    } brenderProgram;

    /*
     * brender.vert/frag as built for what is drawn (see VIDEOI_UseBRenderVariant):
     * - BRENDER_FULL: brenderProgram's, everything
     * - BRENDER_OPAQUE: OPAQUE, nothing cut out (no discard), so the GPU can test the depth before shading: for
     *   what has no colour key's cut-outs, isn't blended or soft, when there are no clip planes
     * - BRENDER_SHADOW: SHADOW_PASS, only what decides the depth (the position and the colour key's cut-outs), for
     *   the sun's shadow maps
     * - BRENDER_SHADOW_OPAQUE: SHADOW_PASS and OPAQUE, only the position: the shadow maps' opaque draws
     * (0 for one that didn't build: BRENDER_FULL instead, BRENDER_SHADOW for BRENDER_SHADOW_OPAQUE)
     */
    brender_variant variants[BRENDER_VARIANTS], *variant; /* (the one in use) */
    /*
     * The BRENDER_FULL and BRENDER_OPAQUE draws' programs built for their features: the code of those they don't
     * need left out (see VIDEOI_UseBRenderFeatures)
     */
    brender_variant featurePrograms[BRENDER_FEATURE_PROGRAMS];
    int featureProgramCount;
    int programListRead, programsBuilt; /* the program list read; its programs built (VIDEOI_BuildListedPrograms) */
    /* the scene's uniforms, given to each program the first time it is used in the scene (see sceneBegin) */
    int sceneSerial;
    float sceneBrightness;
    float sceneSurfaceValues[BR_GL_SCENE_SURFACE_FLOATS];
    /*
     * The GL state the draws of a scene last set (see apply_stored_properties): what is already so isn't set again.
     * -1 for not known (sceneBegin resets it, as anything may have changed it in between)
     */
    struct {
        int colourMask, normalsMask, blend, depthTest, depthMask;
        GLint depthFunc;
        GLint blendFunc[4];
    } drawState;
    const float* sceneLights; /* the scene's lights (spots), as the scene_lighting callback gave them */
    int drawSpots[BR_GL_MAX_SCENE_LIGHTS], drawSpotCount; /* those that reach what is being drawn (see draw_spots) */
    unsigned drawSpotKinds; /* what they are: BRF_LIGHTS, BRF_BLOBS, BRF_LIGHT_HAZE */
    int sceneLightCount;
    int sceneClipPlanes; /* the scene being drawn has clip planes (only BRENDER_FULL clips) */

    GLint sampleBuffers; /* Non-zero if the default framebuffer is multisampled */
    int sceneMultisampled; /* the scene being drawn is (the post-processing's target, or the window): sceneBegin */

    /* The sun's shadow map (see BrGLShadowPassBegin): the scenes drawn while active only write its depth */
    struct {
        int active;
        int level; /* the one being drawn */
        int valid[3]; /* drawn this frame */
        int size[3];
        GLuint fbo[3], tex[3];
        float proj[3][16]; /* the projections they were drawn with (GL) */
        int skip; /* the group being drawn doesn't cast a shadow (translucent) */
    } shadow;

    /* Post-processing of a view (see post.c): rendered offscreen between BrGLPostProcessBegin/End */
    struct {
        int active;
        int width, height, samples;
        GLuint msaaFbo, msaaColour, msaaGlow, msaaDepth, msaaNormal; /* multisampled target, when samples > 0 */
        GLuint resolveFbo, colourTex, glowTex, depthTex;
        GLuint normalTex; /* the surfaces' view space normals (and the sprites' mark), see brender.frag.glsl */
        GLuint aoFbo[3], aoTex[3]; /* ambient occlusion, half resolution (and a target for its blur, and the distance) */
        GLuint aoDepthFbo, aoDepthTex; /* the depth snapshot the ambient occlusion uses, see BrGLPostProcessDepthSnapshot */
        GLuint litFbo, litTex; /* the lit scene, for the blurs (made when first needed), see BrGLPostProcessEnd */
        GLuint sunFbo, sunTex; /* the sun's shadow from its maps, half resolution (made when first needed) */
        GLuint wetFbo, wetTex; /* the wet ground's reflections, half resolution (made when first needed) */
        GLuint dofFbo, dofTex; /* the depth of field's blur, half resolution (made when first needed) */
        GLuint glowHalfFbo, glowHalfTex; /* the bloom and lens flare, half resolution (made when first needed) */
        int depthSnapshot;
        GLuint sceneFbo; /* the framebuffer the scene is being drawn into: msaaFbo, or resolveFbo (see tail) */
        int tail; /* (multisampled) the scene resolved, the rest of it drawn single sampled, see BrGLPostProcessSoftParticles */
        int softParticlesSeen; /* the soft particles were drawn the last frame (then they take the depth snapshot) */
        float softDistance; /* soft particles: what is drawn fades out this close to the snapshot depth, 0 for off */
        float drawSoftDistance; /* that of the models being drawn (kept in their state, see state_stack) */
        int sprites; /* what is drawn is sprites, see BrGLSprites */
        int drawSprite; /* the model being drawn is (kept in its state) */
        GLuint program, aoProgram;
        GLint aoUniforms[16]; /* (see the uniform lists in post.c) */
        /* post.frag built for each pass (stage, see main in post.frag.glsl), program being the first's */
        GLuint stagePrograms[POST_STAGES];
        GLint stageUniforms[POST_STAGES][64];
        GLuint weatherProgram; /* snow and rain particles, see weather.vert.glsl */
        GLint weatherUniforms[24];
        /* the view: the largest perspective scene drawn while active */
        int viewArea;
        float view[4]; /* x, y, width, height in window pixels */
        float proj[2]; /* projection x and y scale */
        float nearZ, farZ;
    } post;
} VIDEO, *HVIDEO;

#pragma pack(push, 16)
/* std140-compatible light structure */
typedef struct shader_data_light {
    /* (X, Y, Z, T), if T == 0, direct, otherwise point/spot */
    alignas(16) br_vector4 position;
    /* (X, Y, Z, 0), normalised */
    alignas(16) br_vector4 direction;
    /* (X, Y, Z, 0), normalised */
    alignas(16) br_vector4 half;
    /* (R, G, B, 0) */
    alignas(16) br_vector4 colour;
    /* (intensity, constant, linear, attenutation) */
    alignas(16) br_vector4 iclq;
    /* (inner, outer), if (0.0, 0.0), then this is a point light. */
    alignas(16) br_vector2 spot_angles;

    /* Pad out the structure to maintain alignment. */
    alignas(4) float _pad0, _pad1;
} shader_data_light;
BR_STATIC_ASSERT(sizeof(shader_data_light) % 16 == 0, "shader_data_light is not aligned");

typedef struct shader_data_scene {
    alignas(16) br_vector4 eye_view;
    alignas(16) shader_data_light lights[BR_MAX_LIGHTS];
    alignas(4) uint32_t num_lights;

    alignas(16) br_vector4 clip_planes[BR_MAX_CLIP_PLANES];
    alignas(4) uint32_t num_clip_planes;
    alignas(4) float hither_z;
    alignas(4) float yon_z;

} shader_data_scene;
BR_STATIC_ASSERT(sizeof(((shader_data_scene*)NULL)->lights) == sizeof(shader_data_light) * BR_MAX_LIGHTS,
    "std::array<shader_data_light> fucked up");

typedef struct shader_data_model {
    alignas(16) br_matrix4 model_view;
    alignas(16) br_matrix4 projection;
    alignas(16) br_matrix4 projection_brender;
    alignas(16) br_matrix4 mvp;
    alignas(16) br_matrix4 normal_matrix;
    alignas(16) br_matrix4 environment_matrix;
    alignas(16) br_matrix4 map_transform;
    alignas(16) br_vector4 surface_colour;
    alignas(16) br_vector4 clear_colour;
    alignas(16) br_vector4 eye_m;
    alignas(4) float ka;
    alignas(4) float ks;
    alignas(4) float kd;
    alignas(4) float power;
    alignas(4) uint32_t lighting;
    alignas(4) uint32_t uv_source;
    alignas(4) uint32_t disable_colour_key;
    alignas(4) uint32_t disable_texture;
    alignas(4) uint32_t fog_enabled;
    alignas(16) br_vector3 fog_colour;
    alignas(4) float fog_min;
    alignas(4) float fog_max;
    alignas(4) float alpha;
    alignas(4) uint32_t prelit;


} shader_data_model;
#pragma pack(pop)

#ifdef __cplusplus
};
#endif
#endif /* VIDEO_H_ */
