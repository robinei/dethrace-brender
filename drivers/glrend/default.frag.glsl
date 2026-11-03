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
// The quad spans uLayerWidth overlay pixels, starting uLayerMargin pixels left of the overlay (0 = just the overlay)
uniform float uLayerWidth;
uniform float uLayerMargin;
// Overlay regions (left, top, right, bottom in overlay pixels) drawn moved right by uRegionShifts pixels
#define MAX_REGIONS 48
uniform int uRegionCount;
uniform vec4 uRegions[MAX_REGIONS];
uniform float uRegionShifts[MAX_REGIONS];

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

bool inRect(vec2 p, vec4 r)
{
    return p.x >= r.x && p.x < r.z && p.y >= r.y && p.y < r.w;
}

void main()
{
    vec2 st = vec2(uv.x, abs(uFlipVertically - uv.y));
    ivec2 size = textureSize(uSampler, 0);

    if (uRegionCount > 0) {
        // Find which overlay pixel this is: one of the moved regions, or the overlay where nothing moved away
        vec2 p = vec2(st.x * uLayerWidth - uLayerMargin, st.y * float(size.y));
        vec2 band = max(fwidth(p), vec2(1e-5));
        vec2 q = p;
        bool found = false;
        for (int i = 0; i < uRegionCount; i++) {
            vec2 moved = p - vec2(uRegionShifts[i], 0.0);
            if (inRect(moved, uRegions[i])) {
                q = moved;
                found = true;
                break;
            }
        }
        if (!found) {
            if (p.x < 0.0 || p.x >= float(size.x)) {
                discard;
            }
            for (int i = 0; i < uRegionCount; i++) {
                if (inRect(p, uRegions[i])) {
                    discard;
                }
            }
        }
        if (uSharp == 1) {
            vec2 seam = floor(q + 0.5);
            vec2 snapped = seam + clamp((q - seam) / band, -0.5, 0.5) - 0.5;
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
        mainColour = texelFetch(uSampler, clamp(ivec2(floor(q)), ivec2(0), size - 1), 0);
        if (uDiscardPurplePixels == 1 && mainColour.rgb == vec3(1.0, 0.0, 1.0)) {
            discard;
        }
        return;
    }

    if (uSharp == 1) {
        // Anti-aliased nearest: flat texels with a ~1 screen pixel blend at texel borders, so pixels scaled
        // by a non-integer factor all look the same width. Blending is done here on premultiplied texels,
        // so the transparent purple never bleeds into its neighbours.
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
