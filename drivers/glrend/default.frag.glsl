##ifdef GL_ES
#version 300 es
precision mediump float;
precision mediump int;
precision lowp usampler2D;
##endif
##ifdef GL_CORE
#version 140
#extension GL_ARB_explicit_attrib_location:require
##endif

in vec3 colour;
in vec2 uv;

uniform sampler2D uSampler;
uniform float uFlipVertically;
uniform int uDiscardPurplePixels;
uniform int uSharp;

layout (location = 0) out vec4 mainColour;

// One texel, premultiplied: purple is transparent when discarding purple pixels
vec4 fetchTexel(ivec2 p, ivec2 size)
{
    vec4 c = texelFetch(uSampler, clamp(p, ivec2(0), size - 1), 0);
    if (uDiscardPurplePixels == 1 && c.rgb == vec3(1.0, 0.0, 1.0)) {
        return vec4(0.0);
    }
    return vec4(c.rgb, 1.0);
}

void main()
{
    vec2 st = vec2(uv.x, abs(uFlipVertically - uv.y));

    if (uSharp == 1) {
        // Anti-aliased nearest: flat texels with a ~1 screen pixel blend at texel borders, so pixels scaled
        // by a non-integer factor all look the same width. Blending is done here on premultiplied texels,
        // so the transparent purple never bleeds into its neighbours.
        ivec2 size = textureSize(uSampler, 0);
        vec2 texel = st * vec2(size);
        vec2 seam = floor(texel + 0.5);
        vec2 band = max(fwidth(texel), vec2(1e-5));
        vec2 snapped = seam + clamp((texel - seam) / band, -0.5, 0.5) - 0.5;
        ivec2 base = ivec2(floor(snapped));
        vec2 f = snapped - floor(snapped);
        vec4 c = mix(mix(fetchTexel(base, size), fetchTexel(base + ivec2(1, 0), size), f.x),
                     mix(fetchTexel(base + ivec2(0, 1), size), fetchTexel(base + ivec2(1, 1), size), f.x), f.y);
        if (c.a <= 0.0) {
            discard;
        }
        mainColour = c;
        return;
    }

    mainColour = texture(uSampler, st);
    if (uDiscardPurplePixels == 1 && mainColour.rgb == vec3(1.0, 0.0, 1.0)) {
        discard;
    }
}
