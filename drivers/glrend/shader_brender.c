#include "drv.h"

#include "brassert.h"
#include "brender.frag.glsl.h"
#include "brender.vert.glsl.h"

static void get_uniforms(GLuint program, brender_uniforms* u) {
    u->main_texture = glGetUniformLocation(program, "main_texture");
    u->sharp_filter = glGetUniformLocation(program, "sharp_filter");
    u->alpha_to_coverage = glGetUniformLocation(program, "alpha_to_coverage");
    u->fog_distance = glGetUniformLocation(program, "fog_distance");
    u->glow_texture = glGetUniformLocation(program, "glow_texture");
    u->glow_intensity = glGetUniformLocation(program, "glow_intensity");
    u->surface_reflect = glGetUniformLocation(program, "surface_reflect");
    u->soft_depth = glGetUniformLocation(program, "soft_depth");
    u->soft_distance = glGetUniformLocation(program, "soft_distance");
    u->surface_wear = glGetUniformLocation(program, "surface_wear");
    u->surface_sprite = glGetUniformLocation(program, "surface_sprite");
    u->surface_soft = glGetUniformLocation(program, "surface_soft");
    u->scene_brightness = glGetUniformLocation(program, "scene_brightness");
    u->num_spots = glGetUniformLocation(program, "num_spots");
    u->spots = glGetUniformLocation(program, "spots");
    u->num_draw_spots = glGetUniformLocation(program, "num_draw_spots");
    u->draw_spots = glGetUniformLocation(program, "draw_spots");
    u->surface_sun = glGetUniformLocation(program, "surface_sun");
    u->surface_up = glGetUniformLocation(program, "surface_up");
    u->surface_haze = glGetUniformLocation(program, "surface_haze");
}

/*
 * brender.vert/frag built with `defines`, its attributes where brenderProgram has them (the geometry's vertex arrays
 * are set up for those) and its uniform blocks at the same bindings
 */
static GLuint compile_variant(HVIDEO hVideo, const char* name, const char* defines, brender_uniforms* u) {
    static const char* attributes[] = { "aPosition", "aUV", "aNormal", "aColour" };
    GLint locations[4];
    GLuint vert, frag, program;
    GLint status;
    GLuint block;
    int i;

    locations[0] = hVideo->brenderProgram.attributes.aPosition;
    locations[1] = hVideo->brenderProgram.attributes.aUV;
    locations[2] = hVideo->brenderProgram.attributes.aNormal;
    locations[3] = hVideo->brenderProgram.attributes.aColour;
    vert = VIDEOI_CreateAndCompileShaderDefines(name, GL_VERTEX_SHADER, BRENDER_VERT_GLSL, sizeof(BRENDER_VERT_GLSL), defines);
    frag = VIDEOI_CreateAndCompileShaderDefines(name, GL_FRAGMENT_SHADER, BRENDER_FRAG_GLSL, sizeof(BRENDER_FRAG_GLSL), defines);
    if (!vert || !frag) {
        glDeleteShader(vert);
        glDeleteShader(frag);
        return 0;
    }
    program = glCreateProgram();
    glAttachShader(program, vert);
    glAttachShader(program, frag);
    for (i = 0; i < 4; i++) {
        if (locations[i] >= 0) {
            glBindAttribLocation(program, locations[i], attributes[i]);
        }
    }
    glLinkProgram(program);
    glDeleteShader(vert);
    glDeleteShader(frag);
    status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        BR_WARNING2("VIDEO: Error linking %s:\n%s", name, log);
        glDeleteProgram(program);
        return 0;
    }
    /* (a variant may have dropped a block it doesn't use) */
    block = glGetUniformBlockIndex(program, "br_scene_state");
    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, block, hVideo->brenderProgram.blockBindingScene);
    }
    block = glGetUniformBlockIndex(program, "br_model_state");
    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(program, block, hVideo->brenderProgram.blockBindingModel);
    }
    get_uniforms(program, u);
    GL_CHECK_ERROR();
    return program;
}

static void VIDEOI_GetShaderVariables(HVIDEO hVideo) {
    glGenBuffers(1, &hVideo->brenderProgram.uboScene);
    glBindBuffer(GL_UNIFORM_BUFFER, hVideo->brenderProgram.uboScene);
    glUniformBlockBinding(hVideo->brenderProgram.program, hVideo->brenderProgram.blockIndexScene,
        hVideo->brenderProgram.blockBindingScene);
    glBindBufferBase(GL_UNIFORM_BUFFER, hVideo->brenderProgram.blockBindingScene, hVideo->brenderProgram.uboScene);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(shader_data_scene), NULL, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &hVideo->brenderProgram.uboModel);
    glBindBuffer(GL_UNIFORM_BUFFER, hVideo->brenderProgram.uboModel);
    glUniformBlockBinding(hVideo->brenderProgram.program, hVideo->brenderProgram.blockIndexModel,
        hVideo->brenderProgram.blockBindingModel);
    glBindBufferBase(GL_UNIFORM_BUFFER, hVideo->brenderProgram.blockBindingModel, hVideo->brenderProgram.uboModel);
    /* (room for a few thousand draws) */
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &hVideo->brenderProgram.uboModelAlign);
    if (hVideo->brenderProgram.uboModelAlign < 1) {
        hVideo->brenderProgram.uboModelAlign = 256;
    }
    hVideo->brenderProgram.uboModelSize = 4 * 1024 * 1024;
    hVideo->brenderProgram.uboModelUsed = 0;
    glBufferData(GL_UNIFORM_BUFFER, hVideo->brenderProgram.uboModelSize, NULL, GL_STREAM_DRAW);

    hVideo->brenderProgram.attributes.aPosition = glGetAttribLocation(hVideo->brenderProgram.program, "aPosition");
    hVideo->brenderProgram.attributes.aUV = glGetAttribLocation(hVideo->brenderProgram.program, "aUV");
    hVideo->brenderProgram.attributes.aNormal = glGetAttribLocation(hVideo->brenderProgram.program, "aNormal");
    hVideo->brenderProgram.attributes.aColour = glGetAttribLocation(hVideo->brenderProgram.program, "aColour");
    hVideo->variants[BRENDER_FULL].program = hVideo->brenderProgram.program;
    get_uniforms(hVideo->brenderProgram.program, &hVideo->variants[BRENDER_FULL].uniforms);
    GL_CHECK_ERROR();
}

/* Draws with brender.vert/frag as built for `which` (BRENDER_*, or BRENDER_FULL if that didn't build) */
void VIDEOI_UseBRenderVariant(HVIDEO hVideo, int which) {
    if (which == BRENDER_SHADOW_OPAQUE && hVideo->variants[which].program == 0) {
        which = BRENDER_SHADOW;
    }
    if (hVideo->variants[which].program == 0) {
        which = BRENDER_FULL;
    }
    if (hVideo->variant != &hVideo->variants[which]) {
        hVideo->variant = &hVideo->variants[which];
        glUseProgram(hVideo->variant->program);
    }
}

br_boolean VIDEOI_CompileBRenderShader(HVIDEO hVideo, const char* vertPath, const char* fragPath) {
    GLuint vert, frag;

    hVideo->brenderProgram.mainTextureBinding = 0;
    hVideo->brenderProgram.blockBindingScene = 1;
    hVideo->brenderProgram.blockBindingModel = 2;

    {
#define _MAX(a, b) ((a) > (b) ? (a) : (b))
        int neededSize = _MAX(sizeof(shader_data_scene), sizeof(shader_data_model));
#undef _MAX
        if (hVideo->maxUniformBlockSize < neededSize) {
            BR_FATAL2("VIDEO: GL_MAX_UNIFORM_BLOCK_SIZE too small, got %d, needed %d.", hVideo->maxUniformBlockSize,
                neededSize);
            return BR_FALSE;
        }
    }

    if (hVideo->maxUniformBufferBindings < 2) {
        BR_FATAL1("VIDEO: GL_MAX_UNIFORM_BUFFER_BINDINGS too small, got %d, needed 2.", hVideo->maxUniformBufferBindings);
        return BR_FALSE;
    }

    /* br_model_state */
    if (hVideo->maxVertexUniformBlocks < 1) {
        BR_FATAL1("VIDEO: GL_MAX_VERTEX_UNIFORM_BLOCKS too small, got %d, needed 1.", hVideo->maxVertexUniformBlocks);
        return BR_FALSE;
    }

    /* br_model_state, br_scene_state */
    if (hVideo->maxFragmentUniformBlocks < 2) {
        BR_FATAL1("VIDEO: GL_MAX_FRAGMENT_UNIFORM_BLOCKS too small, got %d, needed 2.", hVideo->maxFragmentUniformBlocks);
        return BR_FALSE;
    }

    vert = VIDEOI_CreateAndCompileShader("brender.vert", GL_VERTEX_SHADER, BRENDER_VERT_GLSL, sizeof(BRENDER_VERT_GLSL));
    if (!vert)
        return BR_FALSE;

    frag = VIDEOI_CreateAndCompileShader("brender.frag", GL_FRAGMENT_SHADER, BRENDER_FRAG_GLSL, sizeof(BRENDER_FRAG_GLSL));
    if (!frag) {
        glDeleteShader(vert);
        return BR_FALSE;
    }

    if (!(hVideo->brenderProgram.program = VIDEOI_CreateAndCompileProgram(vert, frag))) {
        glDeleteShader(vert);
        glDeleteShader(frag);
        return BR_FALSE;
    }

    glDeleteShader(vert);
    glDeleteShader(frag);

    if (hVideo->brenderProgram.program) {
        hVideo->brenderProgram.blockIndexScene = glGetUniformBlockIndex(hVideo->brenderProgram.program,
            "br_scene_state");
        if (hVideo->brenderProgram.blockIndexScene == GL_INVALID_INDEX) {
            BR_FATAL("VIDEO: Unable to retrieve block index for uniform block 'br_scene_state'.");
            return BR_FALSE;
        }

        hVideo->brenderProgram.blockIndexModel = glGetUniformBlockIndex(hVideo->brenderProgram.program,
            "br_model_state");
        if (hVideo->brenderProgram.blockIndexModel == GL_INVALID_INDEX) {
            BR_FATAL("VIDEO: Unable to retrieve block index for uniform block 'br_model_state'.");
            return BR_FALSE;
        }

        VIDEOI_GetShaderVariables(hVideo);
        hVideo->variant = &hVideo->variants[BRENDER_FULL];

        /* (without them the full program draws it all, the same) */
        hVideo->variants[BRENDER_OPAQUE].program = compile_variant(hVideo, "brender (opaque)", "#define OPAQUE\n",
            &hVideo->variants[BRENDER_OPAQUE].uniforms);
        if (hVideo->variants[BRENDER_OPAQUE].program == 0) {
            BR_WARNING0("VIDEO: no opaque program, everything is drawn with the full one");
        }
        hVideo->variants[BRENDER_SHADOW].program = compile_variant(hVideo, "brender (shadow pass)", "#define SHADOW_PASS\n",
            &hVideo->variants[BRENDER_SHADOW].uniforms);
        if (hVideo->variants[BRENDER_SHADOW].program == 0) {
            BR_WARNING0("VIDEO: no shadow pass program, the shadow maps are drawn with the full one");
        }
        hVideo->variants[BRENDER_SHADOW_OPAQUE].program = compile_variant(hVideo, "brender (shadow pass, opaque)",
            "#define SHADOW_PASS\n#define OPAQUE\n", &hVideo->variants[BRENDER_SHADOW_OPAQUE].uniforms);
    }

    GL_CHECK_ERROR();
    return hVideo->brenderProgram.program != 0;
}
