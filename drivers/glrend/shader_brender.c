#include "drv.h"

#include "brassert.h"
#include "brender.frag.glsl.h"
#include "brender.vert.glsl.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

/* The scene's uniforms (see sceneBegin) into the program in use, the first time it is used in the scene */
static void scene_uniforms(HVIDEO hVideo) {
    brender_variant* v = hVideo->variant;
    const brender_uniforms* u = &v->uniforms;

    if (v->sceneSerial == hVideo->sceneSerial) {
        return;
    }
    v->sceneSerial = hVideo->sceneSerial;
    glUniform1i(u->main_texture, hVideo->brenderProgram.mainTextureBinding);
    glUniform1i(u->glow_texture, 1);
    glUniform1i(u->soft_depth, 2);
    glUniform1f(u->scene_brightness, hVideo->sceneBrightness);
    glUniform1i(u->num_spots, hVideo->sceneLightCount);
    /* (each draw gives those of them that reach it, see StoredGLRenderGroup) */
    glUniform1i(u->num_draw_spots, 0);
    v->drawSpotCount = 0;
    if (hVideo->sceneLightCount > 0) {
        glUniform4fv(u->spots, 3 * hVideo->sceneLightCount, hVideo->sceneLights);
    }
    glUniform4fv(u->surface_sun, 1, &hVideo->sceneSurfaceValues[0]);
    glUniform4fv(u->surface_up, 1, &hVideo->sceneSurfaceValues[4]);
    glUniform4fv(u->surface_haze, 2, &hVideo->sceneSurfaceValues[8]);
    memcpy(v->surfaceUploaded, hVideo->sceneSurfaceValues, sizeof(v->surfaceUploaded));
    glUniform1f(u->glow_intensity, 0.0f);
    glUniform1i(u->alpha_to_coverage, hVideo->sceneMultisampled);
    glUniform1i(u->fog_distance, hVideo->fogDistance != 0);
    /* (the per draw ones: not known) */
    v->glowIntensity = v->reflect = v->wear = v->softDistance = -2.0f;
    v->sharp = v->sprite = v->soft = -1;
    v->alphaToCoverage = hVideo->sceneMultisampled;
}

static void use_variant(HVIDEO hVideo, brender_variant* v) {
    if (hVideo->variant != v) {
        hVideo->variant = v;
        glUseProgram(v->program);
    }
    scene_uniforms(hVideo);
}

/* Draws with brender.vert/frag as built for `which` (BRENDER_*, or BRENDER_FULL if that didn't build) */
void VIDEOI_UseBRenderVariant(HVIDEO hVideo, int which) {
    if (which == BRENDER_SHADOW_OPAQUE && hVideo->variants[which].program == 0) {
        which = BRENDER_SHADOW;
    }
    if (hVideo->variants[which].program == 0) {
        which = BRENDER_FULL;
    }
    use_variant(hVideo, &hVideo->variants[which]);
}

static double seconds(void) {
    struct timespec t;
    timespec_get(&t, TIME_UTC);
    return (double)t.tv_sec + t.tv_nsec * 1e-9;
}

/* Starts building brender.vert/frag for v's base and features: the features (F_*) it doesn't have defined false */
static void build_feature_program(HVIDEO hVideo, brender_variant* v) {
    static const char* defines[BRF_COUNT] = { "F_NIGHT", "F_SHADING", "F_HAZE", "F_FOG", "F_ENV", "F_SHARP",
        "F_SOFT", "F_SOFT_DEPTH", "F_GLOW", "F_WEAR", "F_POST", "F_LIGHTS", "F_BLOBS",
        "F_LIGHT_HAZE" };
    static const char* attributes[] = { "aPosition", "aUV", "aNormal", "aColour" };
    const char* sources[2] = { BRENDER_VERT_GLSL, BRENDER_FRAG_GLSL };
    size_t sizes[2] = { sizeof(BRENDER_VERT_GLSL), sizeof(BRENDER_FRAG_GLSL) };
    GLenum types[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
    GLint locations[4];
    char text[512];
    int i, n;

    n = snprintf(text, sizeof(text), "%s", v->base == BRENDER_OPAQUE ? "#define OPAQUE\n" : "");
    for (i = 0; i < BRF_COUNT; i++) {
        n += snprintf(text + n, sizeof(text) - n, "#define %s %s\n", defines[i], (v->features & (1u << i)) ? "true" : "false");
    }
    locations[0] = hVideo->brenderProgram.attributes.aPosition;
    locations[1] = hVideo->brenderProgram.attributes.aUV;
    locations[2] = hVideo->brenderProgram.attributes.aNormal;
    locations[3] = hVideo->brenderProgram.attributes.aColour;
    v->program = glCreateProgram();
    for (i = 0; i < 2; i++) {
        char* processed = preprocessShader(sources[i], sizes[i], text);
        GLuint s = glCreateShader(types[i]);
        glShaderSource(s, 1, (const GLchar* const*)&processed, NULL);
        glCompileShader(s);
        BrScratchFree(processed);
        glAttachShader(v->program, s);
        /* (deleted with the program) */
        glDeleteShader(s);
    }
    for (i = 0; i < 4; i++) {
        if (locations[i] >= 0) {
            glBindAttribLocation(v->program, locations[i], attributes[i]);
        }
    }
    glLinkProgram(v->program);
    GL_CHECK_ERROR();
}

/* v's program, built (build_feature_program): ready to draw with (1), or it failed (0, and it is never used) */
static int finish_feature_program(HVIDEO hVideo, brender_variant* v) {
    static const char* names[BRF_COUNT] = { "night", "shading", "haze", "fog", "env", "sharp", "soft", "soft-depth",
        "glow", "wear", "post", "lights", "blobs",
        "light-haze" };
    GLint status = GL_FALSE;
    GLuint block;
    char used[160];
    int i, n = 0;

    glGetProgramiv(v->program, GL_LINK_STATUS, &status);
    if (status != GL_TRUE) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(v->program, sizeof(log), &length, log);
        BR_WARNING1("GLREND: a program for some features didn't build (the general one draws instead):\n%s", log);
        glDeleteProgram(v->program);
        v->program = 0;
        return 0;
    }
    block = glGetUniformBlockIndex(v->program, "br_scene_state");
    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(v->program, block, hVideo->brenderProgram.blockBindingScene);
    }
    block = glGetUniformBlockIndex(v->program, "br_model_state");
    if (block != GL_INVALID_INDEX) {
        glUniformBlockBinding(v->program, block, hVideo->brenderProgram.blockBindingModel);
    }
    get_uniforms(v->program, &v->uniforms);
    v->sceneSerial = hVideo->sceneSerial - 1;
    used[0] = '\0';
    for (i = 0; i < BRF_COUNT; i++) {
        if (v->features & (1u << i)) {
            n += snprintf(used + n, sizeof(used) - n, " %s", names[i]);
        }
    }
    BrLogPrintf("GLREND: program %d for features (%s):%s\n", (int)(v - hVideo->featurePrograms) + 1,
        v->base == BRENDER_OPAQUE ? "opaque" : "full", used);
    GL_CHECK_ERROR();
    return 1;
}

/*
 * The program list (programList): the feature programs built in earlier runs, a line each ("brender full|opaque
 * <features in hex>"), added to as others are built. Read once, its programs made ready to be built
 */
static void read_program_list(HVIDEO hVideo) {
    FILE* f;
    char base[16];
    unsigned features;

    hVideo->programListRead = 1;
    if (hVideo->programList[0] == '\0' || (f = fopen(hVideo->programList, "r")) == NULL) {
        return;
    }
    while (hVideo->featureProgramCount < BRENDER_FEATURE_PROGRAMS && fscanf(f, " brender %15s %x", base, &features) == 2) {
        brender_variant* v = &hVideo->featurePrograms[hVideo->featureProgramCount++];
        memset(v, 0, sizeof(*v));
        v->base = strcmp(base, "opaque") == 0 ? BRENDER_OPAQUE : BRENDER_FULL;
        v->features = features & ((1u << BRF_COUNT) - 1);
        v->listed = 1;
    }
    fclose(f);
}

/* v built: into the program list, if it isn't */
static void list_program(HVIDEO hVideo, brender_variant* v) {
    FILE* f;

    if (v->listed || hVideo->programList[0] == '\0') {
        return;
    }
    v->listed = 1;
    if ((f = fopen(hVideo->programList, "a")) != NULL) {
        fprintf(f, "brender %s %x\n", v->base == BRENDER_OPAQUE ? "opaque" : "full", v->features);
        fclose(f);
    }
}

/*
 * Draws with brender.vert/frag built for `base` (BRENDER_FULL or BRENDER_OPAQUE) and `features` (BRF_*): the code
 * of the features the draw doesn't need left out (it was found to cost though not run: by day, the night's code cost
 * the scene a sixth of its time). Built the first time it is needed, so the first draw needing it waits for it (a
 * frame's hitch): those built in earlier runs are listed (programList), and built as the first post-processed
 * scene begins, see VIDEOI_BuildListedPrograms. With the general program for `base` if there is no room for it or
 * it didn't build
 */
void VIDEOI_UseBRenderFeatures(HVIDEO hVideo, int base, unsigned features) {
    brender_variant* v = NULL;
    int i;

    if (!hVideo->programListRead) {
        read_program_list(hVideo);
    }
    for (i = 0; i < hVideo->featureProgramCount; i++) {
        if (hVideo->featurePrograms[i].features == features && hVideo->featurePrograms[i].base == base) {
            v = &hVideo->featurePrograms[i];
            break;
        }
    }
    if (v == NULL && hVideo->featureProgramCount < BRENDER_FEATURE_PROGRAMS) {
        v = &hVideo->featurePrograms[hVideo->featureProgramCount++];
        memset(v, 0, sizeof(*v));
        v->base = base;
        v->features = features;
    }
    if (v != NULL && v->program == 0 && !v->failed) {
        build_feature_program(hVideo, v);
        if (finish_feature_program(hVideo, v)) {
            list_program(hVideo, v);
        } else {
            v->failed = 1;
        }
    }
    if (v == NULL || v->program == 0) {
        VIDEOI_UseBRenderVariant(hVideo, base);
        return;
    }
    use_variant(hVideo, v);
}

/*
 * The listed programs (see VIDEOI_UseBRenderFeatures) built, once, as the first post-processed scene begins (its
 * targets bound): each with a draw of nothing (scissored to a pixel, writing nothing) into them, as the driver
 * finishes a program for what it draws into at its first draw (Freedreno). vao: one with a triangle
 */
void VIDEOI_BuildListedPrograms(HVIDEO hVideo, GLuint vao) {
    double t = seconds();
    int i, n = 0;
    GLint box[4];
    GLboolean scissor;

    if (hVideo->programsBuilt) {
        return;
    }
    hVideo->programsBuilt = 1;
    if (!hVideo->programListRead) {
        read_program_list(hVideo);
    }
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, box);
    glEnable(GL_SCISSOR_TEST);
    glScissor(0, 0, 1, 1);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(vao);
    glBindBufferRange(GL_UNIFORM_BUFFER, hVideo->brenderProgram.blockBindingModel, hVideo->brenderProgram.uboModel,
        0, sizeof(shader_data_model));
    for (i = 0; i < hVideo->featureProgramCount; i++) {
        brender_variant* v = &hVideo->featurePrograms[i];
        if (v->program != 0 || v->failed) {
            continue;
        }
        build_feature_program(hVideo, v);
        if (!finish_feature_program(hVideo, v)) {
            v->failed = 1;
            continue;
        }
        use_variant(hVideo, v);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        n++;
    }
    glBindVertexArray(0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glScissor(box[0], box[1], box[2], box[3]);
    if (!scissor) {
        glDisable(GL_SCISSOR_TEST);
    }
    /* (what the draws set, not known) */
    memset(&hVideo->drawState, 0xff, sizeof(hVideo->drawState));
    if (n > 0) {
        BrLogPrintf("GLREND: %d listed programs built in %.0f ms\n", n, (seconds() - t) * 1000.0);
    }
    GL_CHECK_ERROR();
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
