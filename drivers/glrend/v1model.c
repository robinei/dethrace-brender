/*
 * Support routines for rendering models
 */
#include "brassert.h"
#include "drv.h"
#include "mmpx.h"
#include "xbr.h"
#include <string.h>

static void apply_blend_mode(state_stack* self) {
    /* C_result = (C_source * F_Source) + (C_dest * F_dest) */

    /* NB: srcAlpha and dstAlpha are all GL_ONE and GL_ZERO respectively. */
    switch (self->prim.blend_mode) {
    default:
        /* fallthrough */
    case BRT_BLEND_STANDARD:
        /* fallthrough */
    case BRT_BLEND_DIMMED:
        /*
         * 3dfx blending mode = 1
         * Colour = (alpha * src) + ((1 - alpha) * dest)
         * Alpha  = (1     * src) + (0           * dest)
         */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        break;

    case BRT_BLEND_SUMMED:
        /*
         * 3fdx blending mode = 4
         * Colour = (alpha * src) + (1 * dest)
         * Alpha  = (1     * src) + (0 * dest)
         */
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
        break;

    case BRT_BLEND_PREMULTIPLIED:
        /*
         * 3dfx qblending mode = 2
         * Colour = (1 * src) + ((1 - alpha) * dest)
         * Alpha  = (1 * src) + (0           * dest)
         */
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
        break;
    }
    GL_CHECK_ERROR();
}

static void apply_depth_properties(state_stack* state, uint32_t states) {
    br_boolean depth_valid = BR_TRUE; /* Defaulting to BR_TRUE to keep existing behaviour. */
    GLenum depth_test = GL_NONE;

    /* Only use the states we want (if valid). */
    states = state->valid & states;

    if (states & MASK_STATE_OUTPUT) {
        depth_valid = state->output.depth != NULL;
    }

    if (states & MASK_STATE_SURFACE) {
        if (state->surface.force_front || state->surface.force_back)
            depth_test = GL_FALSE;
        else
            depth_test = GL_TRUE;
    }

    if (depth_valid == BR_TRUE) {
        if (depth_test == GL_TRUE)
            glEnable(GL_DEPTH_TEST);
        else if (depth_test == GL_FALSE)
            glDisable(GL_DEPTH_TEST);
    }

    if (states & MASK_STATE_PRIMITIVE) {
        if (state->prim.flags & PRIMF_DEPTH_WRITE)
            glDepthMask(GL_TRUE);
        else
            glDepthMask(GL_FALSE);

        GLenum depthFunc;
        switch (state->prim.depth_test) {
        case BRT_LESS:
            depthFunc = GL_LESS;
            break;
        case BRT_GREATER:
            depthFunc = GL_GREATER;
            break;
        case BRT_LESS_OR_EQUAL:
            depthFunc = GL_LEQUAL;
            break;
        case BRT_GREATER_OR_EQUAL:
            depthFunc = GL_GEQUAL;
            break;
        case BRT_EQUAL:
            depthFunc = GL_EQUAL;
            break;
        case BRT_NOT_EQUAL:
            depthFunc = GL_NOTEQUAL;
            break;
        case BRT_NEVER:
            depthFunc = GL_NEVER;
            break;
        case BRT_ALWAYS:
            depthFunc = GL_ALWAYS;
            break;
        default:
            depthFunc = GL_LESS;
        }
        glDepthFunc(depthFunc);
    }
    GL_CHECK_ERROR();
}

// take a pixelmap and palette and convert 8 bit to 32 bit just in time
static void update_paletted_texture(HVIDEO hVideo, br_pixelmap *src, br_uint_32 *palette) {
    int width = src->width, height = src->height;
    uint32_t* buffer = BrScratchAllocate(sizeof(uint32_t) * width * height);
    uint32_t* buffer_ptr = buffer;
    br_uint_8* src_px = src->pixels;

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            int index = src_px[y * src->row_bytes + x];
            uint32_t rgb = BR_BLU(palette[index]) << 16 | BR_GRN(palette[index]) << 8 | BR_RED(palette[index]);
            // Black is the colour key. Store it as zero alpha too, so mipmap levels carry
            // coverage and the texture is effectively premultiplied (see brender.frag.glsl).
            *buffer_ptr = rgb == 0 ? 0 : (0xff000000 | rgb);
            buffer_ptr++;
        }
    }

    if (hVideo->textureUpscaler == 1 || hVideo->textureUpscaler == 2) {
        // Magnify 2x with MMPX (smoother diagonals and curves using only the texture's own colours)
        // or xBR (smoother, blended edges).
        uint32_t* upscaled = BrMemAllocate(sizeof(uint32_t) * width * height * 4, BR_MEMORY_DRIVER);
        if (hVideo->textureUpscaler == 2) {
            XBRScale2x(buffer, upscaled, width, height);
        } else {
            MMPXScale2x(buffer, upscaled, width, height);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width * 2, height * 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, upscaled);
        BrMemFree(upscaled);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
    }
    glGenerateMipmap(GL_TEXTURE_2D);
    BrScratchFree(buffer);
    GL_CHECK_ERROR();
}

static void apply_stored_properties(HVIDEO hVideo, state_stack* state, uint32_t states, shader_data_model* model, GLuint tex_default) {
    br_boolean blending_on;

    /* Only use the states we want (if valid). */
    states = state->valid & states;

    if (states & MASK_STATE_CULL) {
        /*
         * Apply culling states. These are a bit confusing:
         * BRT_ONE_SIDED - Simple, cull back faces. From BRT_ONE_SIDED.
         *
         * BRT_TWO_SIDED - This means the face is two-sided, not to cull
         *                 both sides. From BR_MATF_TWO_SIDED. In the .3ds file
         *                 format, the "two sided" flag means the material is
         *                 visible from the back, or "not culled". fmt/load3ds.c
         *                 sets BR_MATF_TWO_SIDED if this is set, so assume this is
         *                 the correct behaviour.
         *
         * BRT_NONE      - Confusing, this is set if the material has
         *                 BR_MATF_ALWAYS_VISIBLE, but is overridden if
         *                 BR_MATF_TWO_SIDED is set. Assume it means the same
         *                 as BR_MATF_TWO_SIDED.
         */
        switch (state->cull.type) {
        case BRT_ONE_SIDED:
        default: /* Default BRender policy, so default. */
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            break;

        case BRT_TWO_SIDED:
        case BRT_NONE:
            glDisable(GL_CULL_FACE);
            break;
        }
    }

    if (states & MASK_STATE_SURFACE) {
        glActiveTexture(GL_TEXTURE0);

        if (state->surface.colour_source == BRT_SURFACE) {
            br_uint_32 colour = state->surface.colour;
            float r = BR_RED(colour) / 255.0f;
            float g = BR_GRN(colour) / 255.0f;
            float b = BR_BLU(colour) / 255.0f;
            //
            BrVector4Set(&model->surface_colour, r, g, b, state->surface.opacity);
        } else {
            BrVector4Set(&model->surface_colour, 0.0f, 1.0f, 1.0f, state->surface.opacity);
        }

        model->ka = state->surface.ka;
        model->ks = state->surface.ks;
        model->kd = state->surface.kd;
        model->power = state->surface.power;

        switch (state->surface.mapping_source) {
        case BRT_GEOMETRY_MAP:
        default:
            model->uv_source = 0;
            break;

        case BRT_ENVIRONMENT_LOCAL:
            model->uv_source = 1;
            break;

        case BRT_ENVIRONMENT_INFINITE:
            model->uv_source = 2;
            break;
        }

        BrMatrix4Copy23(&model->map_transform, &state->surface.map_transform);

        model->prelit = state->surface.prelighting;
        model->lighting = state->surface.lighting;
    }

    if (states & MASK_STATE_PRIMITIVE) {

        if (state->prim.flags & PRIMF_COLOUR_WRITE)
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        else
            glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        if (state->prim.colour_map) {
            model->disable_colour_key = !(state->prim.flags & PRIMF_COLOUR_KEY);

            glBindTexture(GL_TEXTURE_2D, BufferStoredGLGetTexture(state->prim.colour_map));

            // has the 8 bit color source changed?
            if (state->prim.colour_map->paletted_source_dirty == BR_TRUE) {
                update_paletted_texture(hVideo, state->prim.colour_map->source, state->prim.colour_map->palette_pointer->entries);
                state->prim.colour_map->paletted_source_dirty = BR_FALSE;
                state->prim.colour_map->palette_revision = state->prim.colour_map->palette_pointer->revision;
            }
            // or has the palette changed?
            else if (state->prim.colour_map->palette_pointer != NULL && state->prim.colour_map->palette_revision != state->prim.colour_map->palette_pointer->revision) {
                update_paletted_texture(hVideo, state->prim.colour_map->source, state->prim.colour_map->palette_pointer->entries);
                state->prim.colour_map->palette_revision = state->prim.colour_map->palette_pointer->revision;
            }

            // glUniform1i(hVideo->brenderProgram.uniforms.main_texture, hVideo->brenderProgram.mainTextureBinding);
            model->disable_texture = 0;

        } else {

            // todo?
            model->disable_colour_key = 0;
            glBindTexture(GL_TEXTURE_2D, tex_default);
            model->disable_texture = 1;
            // BrVector4Set(&model->surface_colour, 0, 1, 0, 1);
            // glBindTexture(GL_TEXTURE_2D, 27);
            // glUniform1i(hVideo->brenderProgram.uniforms.main_texture, hVideo->brenderProgram.mainTextureBinding);
        }

        GLenum minFilter, magFilter;
        GLfloat maxAnisotropy;
        GLfloat fade;
        br_token filter = state->prim.filter;
        /*
         * Sharp sampling: unfiltered textures are sampled with hardware bilinear, but the shader
         * snaps the UV to texel centres except for a ~1 pixel band at texel borders.
         */
        br_boolean sharp = hVideo->textureSharp && filter != BRT_LINEAR && state->prim.colour_map != NULL;
        if (sharp) {
            filter = BRT_LINEAR;
        }
        glUniform1i(hVideo->brenderProgram.uniforms.sharp_filter, sharp);
        /*
         * Materials request mipmapping through BR_MATF_MAP_ANTIALIASING (mip_map), while
         * BR_MATF_MIP_INTERPOLATION (mip_filter) asks for blending between levels. Treat
         * either as "use mipmaps", and always blend between levels (trilinear) - the
         * magnification filter still decides between crisp and smooth texels.
         */
        if (state->prim.mip_map == BRT_LINEAR || state->prim.mip_filter == BRT_LINEAR) {
            minFilter = filter == BRT_LINEAR ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR;
            maxAnisotropy = hVideo->maxAnisotropy;
        } else {
            minFilter = filter == BRT_LINEAR ? GL_LINEAR : GL_NEAREST;
            maxAnisotropy = 1.0f;
        }
        magFilter = filter == BRT_LINEAR ? GL_LINEAR : GL_NEAREST;

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, (GLint)minFilter);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, (GLint)magFilter);

        if (GLAD_GL_EXT_texture_filter_anisotropic)
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAnisotropy);

        blending_on = (state->prim.flags & PRIMF_BLEND) || (state->prim.colour_map != NULL && state->prim.colour_map->blended);
        fade = hVideo->distanceFade != NULL ? hVideo->distanceFade() : 0.0f;
        glUniform1f(hVideo->brenderProgram.uniforms.distance_fade, fade);
        glUniform1i(hVideo->brenderProgram.uniforms.alpha_to_coverage, hVideo->sampleBuffers != 0 && (blending_on || fade <= 0.0f));
        if (blending_on) {
            glEnable(GL_BLEND);
            model->alpha = state->prim.alpha_val / 255.0f;
            apply_blend_mode(state);
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        } else if (fade > 0.0f) {
            /* Opaque, fading out with distance: output alpha is the fade, so blend instead of using it as
             * colour-key coverage (cut-outs are then discarded, per sample when supersampling) */
            glEnable(GL_BLEND);
            glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
            model->alpha = 1.0f;
            glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        } else {
            glDisable(GL_BLEND);
            /* Opaque: output alpha is only used as colour-key coverage (see brender.frag.glsl) */
            model->alpha = 1.0f;
            if (hVideo->sampleBuffers) {
                glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
            }
        }

        model->fog_enabled = state->prim.fog_enabled;
        BrVector3Set(&model->fog_colour, BR_RED(state->prim.fog_colour) / 255.0f, BR_GRN(state->prim.fog_colour) / 255.0f, BR_BLU(state->prim.fog_colour) / 255.0f);
        model->fog_min = state->prim.fog_min;
        model->fog_max = state->prim.fog_max;
    }
    apply_depth_properties(state, states);
    GL_CHECK_ERROR();
}

void StoredGLRenderGroup(br_geometry_stored* self, br_renderer* renderer, const gl_groupinfo* groupinfo) {
    state_cache* cache = &renderer->state.cache;
    br_device_pixelmap* screen = renderer->pixelmap->screen;
    br_device_pixelmap* colour_target = renderer->state.current->output.colour;
    HVIDEO hVideo = &screen->asFront.video;
    br_renderer_state_stored* stored = groupinfo->stored;
    br_boolean unlit;
    shader_data_model model;

    ASSERT(screen->use_type == BRT_NONE);
    ASSERT(colour_target != NULL);
    ASSERT(colour_target->use_type == BRT_OFFSCREEN);

    /* Update the per-model cache (matrices and lights) */
    StateGLUpdateModel(cache, &renderer->state.current->matrix);

#if DEBUG
    { /* Check that sceneBegin() actually did it's shit. */

        /* Program */
        GLint p;
        glGetIntegerv(GL_CURRENT_PROGRAM, &p);
        ASSERT(p == hVideo->brenderProgram.program);

        /* FBO */
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &p);
        ASSERT(p == renderer->state.current->output.colour->asBack.glFbo);

        /* Model UBO */
        glGetIntegerv(GL_UNIFORM_BUFFER_BINDING, &p);
        ASSERT(p == hVideo->brenderProgram.uboModel);
    }
#endif

    model.projection_brender = cache->model.p_br;
    model.projection = cache->model.p;
    model.model_view = cache->model.mv;
    model.mvp = cache->model.mvp;
    model.normal_matrix = cache->model.normal;
    model.environment_matrix = cache->model.environment;
    model.eye_m = cache->model.eye_m;

    glBindVertexArray(self->gl_vao);

    if (stored) {
        // jeff: disable culling to match behavior of existing hardware drivers
        apply_stored_properties(hVideo, &stored->state, MASK_STATE_PRIMITIVE | MASK_STATE_SURFACE /*| MASK_STATE_CULL */,
            &model, screen->asFront.tex_white);
    } else {
        /* If there's no stored state, apply all states from global. */
        GLuint default_tex;
        if (groupinfo->default_state->state.prim.colour_map) {
            default_tex = groupinfo->default_state->state.prim.colour_map->gl_tex;
        } else {
            default_tex = screen->asFront.tex_white;
        }

        renderer->state.current->surface = groupinfo->default_state->state.surface;
        renderer->state.current->prim = groupinfo->default_state->state.prim;
        renderer->state.current->cull = groupinfo->default_state->state.cull;
        // Jeff: disable culling to match behavior of existing hardware drivers
        apply_stored_properties(hVideo, renderer->state.current, MASK_STATE_PRIMITIVE | MASK_STATE_SURFACE /*| MASK_STATE_CULL */, &model, default_tex);
    }

    BrVector4Set(&model.clear_colour, colour_target->asBack.clearColour[0], colour_target->asBack.clearColour[1],
        colour_target->asBack.clearColour[2], colour_target->asBack.clearColour[3]);

    glBufferData(GL_UNIFORM_BUFFER, sizeof(model), &model, GL_STATIC_DRAW);
    glDrawElements(GL_TRIANGLES, groupinfo->count, GL_UNSIGNED_SHORT, groupinfo->offset);

    renderer->scene_stats.face_group_count++;
    renderer->scene_stats.triangles_rendered_count += groupinfo->group->nfaces;
    renderer->scene_stats.triangles_drawn_count += groupinfo->group->nfaces;
    renderer->scene_stats.vertices_rendered_count += groupinfo->group->nfaces * 3;
    GL_CHECK_ERROR();
}
