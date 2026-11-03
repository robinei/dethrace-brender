/*
 * Post-processing of a view.
 *
 * Between BrGLPostProcessBegin and BrGLPostProcessEnd, everything drawn to the screen (3d scenes, and the
 * 2d layer flushes in between) goes to an offscreen target of the window's size instead, multisampled like
 * the window. End resolves it to colour and depth textures and draws it to the window with the effects:
 *
 * - Depth of field: a disc blur growing with distance, from dof_start to its strongest at dof_end. Samples
 *   only count when they are blurred enough to reach the pixel, so sharp nearer things do not bleed into
 *   the blurred distance around them.
 * - Glow: with glow enabled, the 3d scenes also write their light sources (texels marked by the device's
 *   texture_glow callback) to a second attachment. Its mipmap chain, sampled with a small tent at each
 *   level and added up, makes a soft halo that is added over the image.
 */
#include "drv.h"
#include <brassert.h>

#ifndef GL_SAMPLE_SHADING
#define GL_SAMPLE_SHADING 0x8C36 /* OpenGL 4.0 / ARB_sample_shading */
#endif

static const char post_vert[] =
    "#version 140\n"
    "void main() {\n"
    "    vec2 p = vec2(float((gl_VertexID & 1) << 2) - 1.0, float((gl_VertexID & 2) << 1) - 1.0);\n"
    "    gl_Position = vec4(p, 0.0, 1.0);\n"
    "}\n";

static const char post_frag[] =
    "#version 140\n"
    "uniform sampler2D uColour;\n"
    "uniform sampler2D uGlow;\n"
    "uniform float uGlowStrength;\n"
    "uniform sampler2D uDepth;\n"
    "uniform float uNear;\n"
    "uniform float uFar;\n"
    "uniform float uStart;\n"
    "uniform float uEnd;\n"
    "uniform float uMaxRadius;\n"
    "out vec4 fragColour;\n"
    "const int TAPS = 48;\n"
    "const float GOLDEN_ANGLE = 2.39996323;\n"
    "float blur(vec2 p) {\n"
    "    float d = texelFetch(uDepth, ivec2(p), 0).r;\n"
    "    if (d >= 1.0) {\n"
    "        return 1.0;\n" /* nothing drawn: background, most distant */
    "    }\n"
    "    float z = 2.0 * uNear * uFar / (uFar + uNear - (2.0 * d - 1.0) * (uFar - uNear));\n"
    "    return clamp((z - uStart) / max(uEnd - uStart, 0.0001), 0.0, 1.0);\n"
    "}\n"
    "vec3 bloom(vec2 uv) {\n"
    "    const float weights[7] = float[7](0.12, 0.25, 0.45, 0.55, 0.5, 0.35, 0.2);\n"
    "    vec3 sum = vec3(0.0);\n"
    "    for (int level = 1; level <= 7; level++) {\n"
    "        vec2 t = 0.75 / vec2(textureSize(uGlow, level));\n"
    "        float l = float(level);\n"
    "        vec3 c = textureLod(uGlow, uv + vec2(-t.x, -t.y), l).rgb + textureLod(uGlow, uv + vec2(t.x, -t.y), l).rgb\n"
    "               + textureLod(uGlow, uv + vec2(-t.x, t.y), l).rgb + textureLod(uGlow, uv + vec2(t.x, t.y), l).rgb;\n"
    "        sum += c * 0.25 * weights[level - 1];\n"
    "    }\n"
    "    return sum;\n"
    "}\n"
    "vec3 dof(vec2 p, vec2 size) {\n"
    "    vec3 centre = texelFetch(uColour, ivec2(p), 0).rgb;\n"
    "    float radius = uMaxRadius > 0.0 ? blur(p) * uMaxRadius : 0.0;\n"
    "    if (radius < 0.5) {\n"
    "        return centre;\n"
    "    }\n"
    "    vec3 sum = centre;\n"
    "    float weight = 1.0;\n"
    "    for (int i = 0; i < TAPS; i++) {\n"
    "        float r = sqrt((float(i) + 0.5) / float(TAPS)) * radius;\n"
    "        float a = float(i) * GOLDEN_ANGLE;\n"
    "        vec2 q = p + r * vec2(cos(a), sin(a));\n"
    "        float w = clamp(blur(q) * uMaxRadius - r + 1.0, 0.0, 1.0);\n"
    "        sum += texture(uColour, q / size).rgb * w;\n"
    "        weight += w;\n"
    "    }\n"
    "    return sum / weight;\n"
    "}\n"
    "void main() {\n"
    "    vec2 size = vec2(textureSize(uColour, 0));\n"
    "    vec3 c = dof(gl_FragCoord.xy, size);\n"
    "    if (uGlowStrength > 0.0) {\n"
    "        c += bloom(gl_FragCoord.xy / size) * uGlowStrength;\n"
    "    }\n"
    "    fragColour = vec4(c, 1.0);\n"
    "}\n";

static void delete_targets(HVIDEO hVideo) {
    glDeleteFramebuffers(1, &hVideo->post.msaaFbo);
    glDeleteRenderbuffers(1, &hVideo->post.msaaColour);
    glDeleteRenderbuffers(1, &hVideo->post.msaaGlow);
    glDeleteTextures(1, &hVideo->post.glowTex);
    hVideo->post.msaaGlow = hVideo->post.glowTex = 0;
    glDeleteRenderbuffers(1, &hVideo->post.msaaDepth);
    glDeleteFramebuffers(1, &hVideo->post.resolveFbo);
    glDeleteTextures(1, &hVideo->post.colourTex);
    glDeleteTextures(1, &hVideo->post.depthTex);
    hVideo->post.msaaFbo = hVideo->post.msaaColour = hVideo->post.msaaDepth = 0;
    hVideo->post.resolveFbo = hVideo->post.colourTex = hVideo->post.depthTex = 0;
    hVideo->post.width = hVideo->post.height = 0;
}

static br_error create_targets(HVIDEO hVideo, int width, int height, int samples, int glow) {
    delete_targets(hVideo);

    glGenTextures(1, &hVideo->post.colourTex);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.colourTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(1, &hVideo->post.depthTex);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.depthTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (glow) {
        glGenTextures(1, &hVideo->post.glowTex);
        glBindTexture(GL_TEXTURE_2D, hVideo->post.glowTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

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

static br_boolean compile_program(HVIDEO hVideo) {
    GLuint vert, frag;

    vert = VIDEOI_CreateAndCompileShader("post.vert", GL_VERTEX_SHADER, post_vert, sizeof(post_vert));
    frag = VIDEOI_CreateAndCompileShader("post.frag", GL_FRAGMENT_SHADER, post_frag, sizeof(post_frag));
    if (!vert || !frag) {
        return BR_FALSE;
    }
    hVideo->post.program = VIDEOI_CreateAndCompileProgram(vert, frag);
    glDeleteShader(vert);
    glDeleteShader(frag);
    if (!hVideo->post.program) {
        return BR_FALSE;
    }
    hVideo->post.uColour = glGetUniformLocation(hVideo->post.program, "uColour");
    hVideo->post.uGlow = glGetUniformLocation(hVideo->post.program, "uGlow");
    hVideo->post.uGlowStrength = glGetUniformLocation(hVideo->post.program, "uGlowStrength");
    hVideo->post.uDepth = glGetUniformLocation(hVideo->post.program, "uDepth");
    hVideo->post.uNear = glGetUniformLocation(hVideo->post.program, "uNear");
    hVideo->post.uFar = glGetUniformLocation(hVideo->post.program, "uFar");
    hVideo->post.uStart = glGetUniformLocation(hVideo->post.program, "uStart");
    hVideo->post.uEnd = glGetUniformLocation(hVideo->post.program, "uEnd");
    hVideo->post.uMaxRadius = glGetUniformLocation(hVideo->post.program, "uMaxRadius");
    return BR_TRUE;
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
    if (hVideo->post.program == 0 && !compile_program(hVideo)) {
        return BRE_FAIL;
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
    hVideo->post.active = 1;
    GL_CHECK_ERROR();
    return BRE_OK;
}

void BR_PUBLIC_ENTRY BrGLPostProcessEnd(br_pixelmap* screen, const br_gl_post_params* params) {
    br_device_pixelmap* self = (br_device_pixelmap*)screen;
    HVIDEO hVideo;
    GLboolean sample_shading;
    int w, h;

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

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
    /* one shader invocation per pixel, not per sample */
    sample_shading = glIsEnabled(GL_SAMPLE_SHADING);
    glDisable(GL_SAMPLE_SHADING);

    glUseProgram(hVideo->post.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.colourTex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.depthTex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, hVideo->post.glowTex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(hVideo->post.uColour, 0);
    glUniform1i(hVideo->post.uDepth, 1);
    glUniform1i(hVideo->post.uGlow, 2);
    glUniform1f(hVideo->post.uGlowStrength, hVideo->post.glowTex != 0 ? params->glow_strength : 0.0f);
    glUniform1f(hVideo->post.uNear, params->hither);
    glUniform1f(hVideo->post.uFar, params->yon);
    glUniform1f(hVideo->post.uStart, params->dof_start);
    glUniform1f(hVideo->post.uEnd, params->dof_end);
    glUniform1f(hVideo->post.uMaxRadius, params->dof_max_radius);
    glBindVertexArray(self->asFront.screen_buffer_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);

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
