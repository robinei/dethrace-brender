/*
 * VIDEO structures
 */
#ifndef VIDEO_H_
#define VIDEO_H_

#ifdef __cplusplus
extern "C" {
#endif


#define BR_STATIC_ASSERT(cond, msg) _Static_assert((cond), msg)

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
    br_device_gl_distancefade_cbfn* distanceFade;
    br_device_gl_textureglow_cbfn* textureGlow;
    br_device_gl_glowscale_cbfn* glowScale;
    br_device_gl_scenelighting_cbfn* sceneLighting;

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
    } defaultProgram;

    struct {
        GLuint program;

        struct {
            GLint aPosition; /* Vectex Position, vec3 */
            GLint aUV;       /* UV, vec2 */
            GLint aNormal;   /* Vertex Normal, vec3 */
            GLint aColour;   /* Vertex colour, vec4 */
        } attributes;

        struct {
            GLint main_texture; /* sampler2D */
            GLint alpha_to_coverage; /* bool */
            GLint sharp_filter;      /* bool */
            GLint fog_distance;      /* bool */
            GLint distance_fade;     /* float */
            GLint glow_texture;      /* sampler2D */
            GLint glow_intensity;    /* float */
            GLint scene_brightness;  /* float */
            GLint num_spots;         /* int */
            GLint spots;             /* vec4[3 * MAX] */
        } uniforms;

        GLuint uboScene;
        GLuint blockIndexScene;
        GLuint blockBindingScene;

        GLuint uboModel;
        GLuint blockIndexModel;
        GLuint blockBindingModel;

        GLint mainTextureBinding;
    } brenderProgram;

    GLint sampleBuffers; /* Non-zero if the default framebuffer is multisampled */

    /* Post-processing of a view (see post.c): rendered offscreen between BrGLPostProcessBegin/End */
    struct {
        int active;
        int width, height, samples;
        GLuint msaaFbo, msaaColour, msaaGlow, msaaDepth; /* multisampled target, when samples > 0 */
        GLuint resolveFbo, colourTex, glowTex, depthTex;
        GLuint aoFbo[2], aoTex[2]; /* ambient occlusion, half resolution (and a target for its blur) */
        GLuint program, aoProgram;
        GLint uniforms[32], aoUniforms[8];
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
