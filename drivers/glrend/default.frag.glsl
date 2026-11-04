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
// The moved layer (with uRegionCount > 0): its regions (left, top, right, bottom in overlay pixels) drawn moved
// right by uRegionShifts pixels; anything else in it where it is
#define MAX_REGIONS 64
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

// One texel limited to [lo, hi]; with skip_moved, the texels of moved regions (at their old places) are
// transparent, so the overlay around them doesn't blend in their edges
vec4 regionTexel(ivec2 t, ivec2 lo, ivec2 hi, ivec2 size, bool skip_moved)
{
    t = clamp(t, lo, hi);
    if (skip_moved) {
        vec2 c = vec2(t) + 0.5;
        for (int i = 0; i < uRegionCount; i++) {
            if (uRegionShifts[i] != 0.0 && inRect(c, uRegions[i])) {
                return vec4(0.0);
            }
        }
    }
    return fetchTexel(t, size);
}

// The overlay at q (premultiplied, alpha 0 where transparent), its texels limited to [lo, hi]
vec4 sampleOverlay(vec2 q, vec2 band, ivec2 lo, ivec2 hi, ivec2 size, bool skip_moved)
{
    if (uSharp == 1) {
        vec2 seam = floor(q + 0.5);
        vec2 snapped = seam + clamp((q - seam) / band, -0.5, 0.5) - 0.5;
        ivec2 base = ivec2(floor(snapped));
        vec2 f = snapped - floor(snapped);
        return mix(mix(regionTexel(base, lo, hi, size, skip_moved), regionTexel(base + ivec2(1, 0), lo, hi, size, skip_moved), f.x),
                   mix(regionTexel(base + ivec2(0, 1), lo, hi, size, skip_moved), regionTexel(base + ivec2(1, 1), lo, hi, size, skip_moved), f.x), f.y);
    }
    return regionTexel(ivec2(floor(q)), lo, hi, size, skip_moved);
}

void main()
{
    vec2 st = vec2(uv.x, abs(uFlipVertically - uv.y));
    ivec2 size = textureSize(uSampler, 0);

    if (uRegionCount > 0) {
        // Which layer pixel this is: one of a moved region's, or the layer where nothing moved away
        vec2 p = vec2(st.x * uLayerWidth - uLayerMargin, st.y * float(size.y));
        vec2 band = max(fwidth(p), vec2(1e-5));
        ivec2 whole_hi = size - 1;
        for (int i = 0; i < uRegionCount; i++) {
            vec2 moved = p - vec2(uRegionShifts[i], 0.0);
            if (uRegionShifts[i] != 0.0 && inRect(moved, uRegions[i])) {
                // (only its own texels: blending with the row below its edge drew a line)
                vec4 c = sampleOverlay(moved, band, ivec2(uRegions[i].xy), ivec2(uRegions[i].zw) - 1, size, false);
                if (c.a <= 0.0) {
                    discard;
                }
                mainColour = c;
                return;
            }
        }
        if (p.x < 0.0 || p.x >= float(size.x)) {
            discard;
        }
        bool near_moved = false;
        for (int i = 0; i < uRegionCount; i++) {
            if (uRegionShifts[i] != 0.0) {
                if (inRect(p, uRegions[i])) {
                    discard;
                }
                // (just outside one: leave its texels out of the blend)
                near_moved = near_moved || inRect(p, uRegions[i] + vec4(-1.5, -1.5, 1.5, 1.5));
            }
        }
        vec4 c = sampleOverlay(p, band, ivec2(0), whole_hi, size, near_moved);
        if (c.a <= 0.0) {
            discard;
        }
        mainColour = c;
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
