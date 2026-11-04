/*
 * Post-processing of a view.
 *
 * Between BrGLPostProcessBegin and BrGLPostProcessEnd, everything drawn to the screen (3d scenes, and the
 * 2d layer flushes in between) goes to an offscreen target of the window's size instead, multisampled like
 * the window. The largest perspective scene drawn in between is taken as the view: its viewport, projection
 * and depth range let the effects rebuild view space positions from depth. End resolves the target to colour
 * and depth textures and draws it to the window with the effects (see post.frag.glsl):
 *
 * - Heat shimmer above given points (fires).
 * - Depth of field: a disc blur growing with distance. Samples only count when they are blurred enough to
 *   reach the pixel, so sharp nearer things do not bleed into the blurred distance around them.
 * - Ambient occlusion (ssao.frag.glsl), at half resolution, blurred and upsampled with depth aware weights.
 * - Speed blur: a radial blur towards the edges of the view.
 * - Glow: with glow enabled the 3d scenes also write their light sources (texels marked by the device's
 *   texture_glow callback) to a second attachment. Its mipmap chain makes a bloom and light streaks.
 * - Ice and water mirror the scene (and sky): the surfaces masked in the glow attachment's alpha (as 1 - the
 *   texture_reflect callback's 1 for ice, 0.5 for water), by a ray march through the depth; water with ripples.
 * - Wet ground at night: up-facing surfaces reflect the glowing lights, found by a ray march through the depth.
 * - Colour grading.
 */
#include "drv.h"
#include <brassert.h>
#include "post.vert.glsl.h"
#include "post.frag.glsl.h"
#include "ssao.frag.glsl.h"

#ifndef GL_SAMPLE_SHADING
#define GL_SAMPLE_SHADING 0x8C36 /* OpenGL 4.0 / ARB_sample_shading */
#endif

enum {
    U_COLOUR, U_DEPTH, U_GLOW, U_AO, U_VIEW, U_PROJ, U_NEAR, U_FAR, U_TIME, U_SCALE, U_START, U_END, U_MAX_RADIUS,
    U_GLOW_STRENGTH, U_FLARE_STRENGTH, U_AO_STRENGTH, U_SPEED, U_SHIMMER_COUNT, U_SHIMMERS, U_GRADE, U_AO_DEPTH,
    U_WET, U_UP, U_ICE, U_WATER, U_TO_WORLD,
    U_COUNT
};
static const char* uniform_names[U_COUNT] = {
    "uColour", "uDepth", "uGlow", "uAO", "uView", "uProj", "uNear", "uFar", "uTime", "uScale", "uStart", "uEnd",
    "uMaxRadius", "uGlowStrength", "uFlareStrength", "uAOStrength", "uSpeed", "uShimmerCount", "uShimmers", "uGrade", "uAODepth",
    "uWet", "uUp", "uIce", "uWater", "uToWorld"
};
enum {
    A_DEPTH, A_VIEW, A_PROJ, A_NEAR, A_FAR, A_RADIUS, A_AO, A_PASS,
    A_COUNT
};
static const char* ao_uniform_names[A_COUNT] = { "uDepth", "uView", "uProj", "uNear", "uFar", "uRadius", "uAO", "uPass" };

static void delete_targets(HVIDEO hVideo) {
    glDeleteFramebuffers(1, &hVideo->post.msaaFbo);
    glDeleteRenderbuffers(1, &hVideo->post.msaaColour);
    glDeleteRenderbuffers(1, &hVideo->post.msaaGlow);
    glDeleteRenderbuffers(1, &hVideo->post.msaaDepth);
    glDeleteFramebuffers(1, &hVideo->post.resolveFbo);
    glDeleteTextures(1, &hVideo->post.colourTex);
    glDeleteTextures(1, &hVideo->post.glowTex);
    glDeleteTextures(1, &hVideo->post.depthTex);
    glDeleteFramebuffers(2, hVideo->post.aoFbo);
    glDeleteTextures(2, hVideo->post.aoTex);
    glDeleteFramebuffers(1, &hVideo->post.aoDepthFbo);
    glDeleteTextures(1, &hVideo->post.aoDepthTex);
    hVideo->post.aoDepthFbo = hVideo->post.aoDepthTex = 0;
    hVideo->post.msaaFbo = hVideo->post.msaaColour = hVideo->post.msaaGlow = hVideo->post.msaaDepth = 0;
    hVideo->post.resolveFbo = hVideo->post.colourTex = hVideo->post.glowTex = hVideo->post.depthTex = 0;
    hVideo->post.aoFbo[0] = hVideo->post.aoFbo[1] = hVideo->post.aoTex[0] = hVideo->post.aoTex[1] = 0;
    hVideo->post.width = hVideo->post.height = 0;
}

static GLuint create_texture(GLint internal_format, int width, int height, GLenum format, GLenum type, GLint filter, int mipmaps) {
    GLuint tex;

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, type, NULL);
    if (mipmaps) {
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmaps ? GL_LINEAR_MIPMAP_LINEAR : filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static br_error create_targets(HVIDEO hVideo, int width, int height, int samples, int glow) {
    delete_targets(hVideo);

    hVideo->post.colourTex = create_texture(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, 0);
    hVideo->post.depthTex = create_texture(GL_DEPTH_COMPONENT24, width, height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST, 0);
    if (glow) {
        hVideo->post.glowTex = create_texture(GL_RGBA8, width, height, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR, 1);
    }
    hVideo->post.aoTex[0] = create_texture(GL_R8, (width + 1) / 2, (height + 1) / 2, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST, 0);
    hVideo->post.aoTex[1] = create_texture(GL_R8, (width + 1) / 2, (height + 1) / 2, GL_RED, GL_UNSIGNED_BYTE, GL_NEAREST, 0);
    hVideo->post.aoDepthTex = create_texture(GL_DEPTH_COMPONENT24, width, height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST, 0);

    glGenFramebuffers(1, &hVideo->post.resolveFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.resolveFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hVideo->post.colourTex, 0);
    if (glow) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, hVideo->post.glowTex, 0);
    }
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hVideo->post.depthTex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        BR_WARNING0("GLREND: post-processing resolve framebuffer incomplete");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        delete_targets(hVideo);
        return BRE_FAIL;
    }

    glGenFramebuffers(2, hVideo->post.aoFbo);
    for (int i = 0; i < 2; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.aoFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hVideo->post.aoTex[i], 0);
    }
    glGenFramebuffers(1, &hVideo->post.aoDepthFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.aoDepthFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, hVideo->post.aoDepthTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);

    if (samples > 0) {
        glGenRenderbuffers(1, &hVideo->post.msaaColour);
        glBindRenderbuffer(GL_RENDERBUFFER, hVideo->post.msaaColour);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
        if (glow) {
            glGenRenderbuffers(1, &hVideo->post.msaaGlow);
            glBindRenderbuffer(GL_RENDERBUFFER, hVideo->post.msaaGlow);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
        }
        glGenRenderbuffers(1, &hVideo->post.msaaDepth);
        glBindRenderbuffer(GL_RENDERBUFFER, hVideo->post.msaaDepth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
        glGenFramebuffers(1, &hVideo->post.msaaFbo);
        glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.msaaFbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, hVideo->post.msaaColour);
        if (glow) {
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_RENDERBUFFER, hVideo->post.msaaGlow);
        }
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, hVideo->post.msaaDepth);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            BR_WARNING0("GLREND: post-processing multisampled framebuffer incomplete");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            delete_targets(hVideo);
            return BRE_FAIL;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    hVideo->post.width = width;
    hVideo->post.height = height;
    hVideo->post.samples = samples;
    GL_CHECK_ERROR();
    return BRE_OK;
}

static GLuint compile_program(const char* name, const char* frag_source, size_t frag_size, const char** names, int count, GLint* locations) {
    GLuint vert, frag, program;
    int i;

    vert = VIDEOI_CreateAndCompileShader("post.vert", GL_VERTEX_SHADER, POST_VERT_GLSL, sizeof(POST_VERT_GLSL));
    frag = VIDEOI_CreateAndCompileShader(name, GL_FRAGMENT_SHADER, frag_source, frag_size);
    if (!vert || !frag) {
        return 0;
    }
    program = VIDEOI_CreateAndCompileProgram(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    for (i = 0; program && i < count; i++) {
        locations[i] = glGetUniformLocation(program, names[i]);
    }
    return program;
}

br_error BR_PUBLIC_ENTRY BrGLPostProcessBegin(br_pixelmap* screen, br_boolean glow) {
    br_device_pixelmap* self = (br_device_pixelmap*)screen;
    HVIDEO hVideo;
    int x, y, width, height;
    float sx, sy;
    GLint samples;

    if (self == NULL || self->screen != self) {
        return BRE_FAIL;
    }
    hVideo = &self->asFront.video;
    if (hVideo->post.active) {
        return BRE_OK;
    }
    if (hVideo->post.program == 0) {
        hVideo->post.program = compile_program("post.frag", POST_FRAG_GLSL, sizeof(POST_FRAG_GLSL), uniform_names, U_COUNT, hVideo->post.uniforms);
        hVideo->post.aoProgram = compile_program("ssao.frag", SSAO_FRAG_GLSL, sizeof(SSAO_FRAG_GLSL), ao_uniform_names, A_COUNT, hVideo->post.aoUniforms);
        if (hVideo->post.program == 0 || hVideo->post.aoProgram == 0) {
            return BRE_FAIL;
        }
    }

    DevicePixelmapGLGetViewport(self, &x, &y, &sx, &sy);
    width = 2 * x + (int)(self->pm_width * sx + 0.5f);
    height = 2 * y + (int)(self->pm_height * sy + 0.5f);
    glow = glow ? 1 : 0;
    if (width != hVideo->post.width || height != hVideo->post.height || hVideo->post.resolveFbo == 0
        || glow != (hVideo->post.glowTex != 0)) {
        /* match the window's multisampling */
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        samples = 0;
        if (hVideo->sampleBuffers) {
            glGetIntegerv(GL_SAMPLES, &samples);
        }
        if (create_targets(hVideo, width, height, samples, glow) != BRE_OK) {
            return BRE_FAIL;
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.samples > 0 ? hVideo->post.msaaFbo : hVideo->post.resolveFbo);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(1.0);
    if (hVideo->post.glowTex != 0) {
        static const GLenum both[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
        glDrawBuffers(2, both);
    }
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    {
        /* only the 3d scenes write the glow, see renderer.c */
        static const GLenum colour_only[] = { GL_COLOR_ATTACHMENT0 };
        glDrawBuffers(1, colour_only);
    }
    hVideo->post.viewArea = 0;
    hVideo->post.depthSnapshot = 0;
    hVideo->post.active = 1;
    GL_CHECK_ERROR();
    return BRE_OK;
}

void BR_PUBLIC_ENTRY BrGLPostProcessEnd(br_pixelmap* screen, const br_gl_post_params* params) {
    br_device_pixelmap* self = (br_device_pixelmap*)screen;
    HVIDEO hVideo;
    GLboolean sample_shading;
    GLint* u;
    GLint* a;
    int w, h, ao;
    GLuint ao_depth;
    float near_z, far_z;

    if (self == NULL || self->screen != self) {
        return;
    }
    hVideo = &self->asFront.video;
    if (!hVideo->post.active) {
        return;
    }
    hVideo->post.active = 0;
    w = hVideo->post.width;
    h = hVideo->post.height;
    u = hVideo->post.uniforms;
    a = hVideo->post.aoUniforms;
    if (hVideo->post.viewArea <= 0) {
        /* no perspective scene: the whole window, with the given depth range */
        hVideo->post.view[0] = 0.0f;
        hVideo->post.view[1] = 0.0f;
        hVideo->post.view[2] = (float)w;
        hVideo->post.view[3] = (float)h;
        hVideo->post.proj[0] = hVideo->post.proj[1] = 1.0f;
        hVideo->post.nearZ = params->hither;
        hVideo->post.farZ = params->yon;
    }
    near_z = hVideo->post.nearZ;
    far_z = hVideo->post.farZ;
    ao = params->ao_strength > 0.0f && hVideo->post.viewArea > 0;
    ao_depth = hVideo->post.depthSnapshot ? hVideo->post.aoDepthTex : hVideo->post.depthTex;

    if (hVideo->post.samples > 0) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, hVideo->post.msaaFbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hVideo->post.resolveFbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glDrawBuffer(GL_COLOR_ATTACHMENT0);
        glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT, GL_NEAREST);
        if (hVideo->post.glowTex != 0) {
            glReadBuffer(GL_COLOR_ATTACHMENT1);
            glDrawBuffer(GL_COLOR_ATTACHMENT1);
            glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
        }
    }
    if (hVideo->post.glowTex != 0) {
        glBindTexture(GL_TEXTURE_2D, hVideo->post.glowTex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    /* one shader invocation per pixel, not per sample */
    sample_shading = glIsEnabled(GL_SAMPLE_SHADING);
    glDisable(GL_SAMPLE_SHADING);
    glBindVertexArray(self->asFront.screen_buffer_vao);

    if (ao) {
        /* ambient occlusion into aoTex[0], blurred horizontally into aoTex[1], and vertically back */
        glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.aoFbo[0]);
        glViewport(0, 0, (w + 1) / 2, (h + 1) / 2);
        glUseProgram(hVideo->post.aoProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, ao_depth);
        glUniform1i(a[A_DEPTH], 0);
        glUniform1i(a[A_AO], 1);
        glUniform1i(a[A_PASS], 0);
        glUniform4fv(a[A_VIEW], 1, hVideo->post.view);
        glUniform2fv(a[A_PROJ], 1, hVideo->post.proj);
        glUniform1f(a[A_NEAR], near_z);
        glUniform1f(a[A_FAR], far_z);
        glUniform1f(a[A_RADIUS], params->ao_radius);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        for (int pass = 1; pass <= 2; pass++) {
            glBindFramebuffer(GL_FRAMEBUFFER, hVideo->post.aoFbo[pass & 1]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, hVideo->post.aoTex[(pass - 1) & 1]);
            glUniform1i(a[A_PASS], pass);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glUseProgram(hVideo->post.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.colourTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.depthTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.glowTex);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.aoTex[0]);
    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, ao_depth);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(u[U_COLOUR], 0);
    glUniform1i(u[U_DEPTH], 1);
    glUniform1i(u[U_GLOW], 2);
    glUniform1i(u[U_AO], 3);
    glUniform1i(u[U_AO_DEPTH], 4);
    glUniform4fv(u[U_VIEW], 1, hVideo->post.view);
    glUniform2fv(u[U_PROJ], 1, hVideo->post.proj);
    glUniform1f(u[U_NEAR], near_z);
    glUniform1f(u[U_FAR], far_z);
    glUniform1f(u[U_TIME], params->time);
    glUniform1f(u[U_SCALE], h / 1080.0f);
    glUniform1f(u[U_START], params->dof_start);
    glUniform1f(u[U_END], params->dof_end);
    glUniform1f(u[U_MAX_RADIUS], params->dof_max_radius);
    glUniform1f(u[U_GLOW_STRENGTH], hVideo->post.glowTex != 0 ? params->glow_strength : 0.0f);
    glUniform1f(u[U_FLARE_STRENGTH], params->flare_strength);
    glUniform1f(u[U_AO_STRENGTH], ao ? params->ao_strength : 0.0f);
    glUniform1f(u[U_SPEED], params->speed);
    glUniform1i(u[U_SHIMMER_COUNT], params->shimmer_count < 8 ? params->shimmer_count : 8);
    if (params->shimmer_count > 0) {
        glUniform4fv(u[U_SHIMMERS], params->shimmer_count < 8 ? params->shimmer_count : 8, params->shimmers);
    }
    glUniform2f(u[U_GRADE], params->grade, params->night);
    glUniform1f(u[U_WET], hVideo->post.glowTex != 0 && hVideo->post.viewArea > 0 ? params->wet : 0.0f);
    glUniform3fv(u[U_UP], 1, params->up);
    glUniform1f(u[U_ICE], hVideo->post.glowTex != 0 && hVideo->post.viewArea > 0 ? params->ice : 0.0f);
    glUniform1f(u[U_WATER], hVideo->post.glowTex != 0 && hVideo->post.viewArea > 0 ? params->water : 0.0f);
    glUniform3fv(u[U_TO_WORLD], 4, params->to_world);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    glBindVertexArray(0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glUseProgram(0);
    if (sample_shading) {
        glEnable(GL_SAMPLE_SHADING);
    }
    glEnable(GL_DEPTH_TEST);
    GL_CHECK_ERROR();
}

/* Called by sceneBegin while post-processing: the largest perspective scene is the view */
void BR_PUBLIC_ENTRY BrGLPostProcessDepthSnapshot(br_pixelmap* screen) {
    br_device_pixelmap* self = (br_device_pixelmap*)screen;
    HVIDEO hVideo;
    GLuint scene_fbo;
    GLboolean scissor;
    int w, h;

    if (self == NULL || self->screen != self) {
        return;
    }
    hVideo = &self->asFront.video;
    if (!hVideo->post.active || hVideo->post.aoDepthFbo == 0) {
        return;
    }
    w = hVideo->post.width;
    h = hVideo->post.height;
    scene_fbo = hVideo->post.samples > 0 ? hVideo->post.msaaFbo : hVideo->post.resolveFbo;
    scissor = glIsEnabled(GL_SCISSOR_TEST);
    glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, scene_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, hVideo->post.aoDepthFbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    hVideo->post.depthSnapshot = 1;
    GL_CHECK_ERROR();
}

void PostProcessRecordScene(HVIDEO hVideo, int x, int y, int w, int h, const br_matrix4* view_to_screen, br_token hint,
    br_scalar hither, br_scalar yon) {
    if (!hVideo->post.active || hint != BRT_PERSPECTIVE || w * h <= hVideo->post.viewArea) {
        return;
    }
    hVideo->post.viewArea = w * h;
    hVideo->post.view[0] = (float)x;
    hVideo->post.view[1] = (float)y;
    hVideo->post.view[2] = (float)w;
    hVideo->post.view[3] = (float)h;
    hVideo->post.proj[0] = view_to_screen->m[0][0];
    hVideo->post.proj[1] = view_to_screen->m[1][1];
    hVideo->post.nearZ = hither;
    hVideo->post.farZ = yon;
}
