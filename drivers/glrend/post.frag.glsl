##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
##endif

// Post-processing of a view (see post.c), in this order: heat shimmer, depth of field, ambient occlusion,
// ice, water and wet ground, speed blur, glow (bloom and light streaks), colour grading
uniform sampler2D uColour;
uniform sampler2D uDepth;
uniform sampler2D uGlow;
uniform sampler2D uAO;
uniform sampler2D uAODepth; // the depth the ambient occlusion was made from (drawn before sprites and smoke)
uniform vec4 uView;
uniform vec2 uProj;
uniform float uNear;
uniform float uFar;
uniform float uTime;
uniform float uScale;        // window height / 1080
// depth of field
uniform float uStart;
uniform float uEnd;
uniform float uMaxRadius;
// glow
uniform float uGlowStrength;
uniform float uFlareStrength;
// ambient occlusion
uniform float uAOStrength;
// speed blur, 0..1
uniform float uSpeed;
// heat shimmer: view space position (xyz) and strength (w) of each
#define MAX_SHIMMERS 8
uniform int uShimmerCount;
uniform vec4 uShimmers[MAX_SHIMMERS];
// colour grading: strength, night
uniform vec2 uGrade;
// wet ground: strength of the reflections of the glowing lights, 0 for none; the world's up in view space
uniform float uWet;
uniform vec3 uUp;
// ice: strength of its mirror image (of the scene), 0 for none
uniform float uIce;
// water: strength of its mirror image; view to world space (rows: the x, y and z axes, the position)
uniform float uWater;
uniform vec3 uToWorld[4];
// car paint: strength of its glossy reflections
uniform float uPaint;
// sun shadows: the sun's direction in view space, and their strength; the sun's shadow maps (levels, each
// covering more than the one before): their projections, world to their cameras (rows: x, y, z, position, for
// each), their texel sizes (0 for none this frame)
uniform vec4 uSun;
uniform sampler2D uShadowMap[3];
uniform mat4 uShadowProj[3];
uniform vec3 uShadowView[12];
uniform float uShadowTexel[3];
// the strength of the scene's sun shading (see brender.frag.glsl)
uniform float uSunShading;
// the surfaces' view space normals (and the sprites' mark), from the 3d scenes (see brender.frag.glsl)
uniform sampler2D uNormal;

out vec4 fragColour;

const int TAPS = 48;
const float GOLDEN_ANGLE = 2.39996323;

float linearDepth(float d) {
    return 2.0 * uNear * uFar / (uFar + uNear - (2.0 * d - 1.0) * (uFar - uNear));
}

float blur(vec2 p) {
    float d = texelFetch(uDepth, ivec2(p), 0).r;
    if (d >= 1.0) {
        return 1.0; // nothing drawn: background, most distant
    }
    return clamp((linearDepth(d) - uStart) / max(uEnd - uStart, 0.0001), 0.0, 1.0);
}

vec3 dof(vec2 p, vec2 size) {
    vec3 centre = texelFetch(uColour, ivec2(p), 0).rgb;
    float radius = uMaxRadius > 0.0 ? blur(p) * uMaxRadius : 0.0;
    if (radius < 0.5) {
        return centre;
    }
    vec3 sum = centre;
    float weight = 1.0;
    for (int i = 0; i < TAPS; i++) {
        float r = sqrt((float(i) + 0.5) / float(TAPS)) * radius;
        float a = float(i) * GOLDEN_ANGLE;
        vec2 q = p + r * vec2(cos(a), sin(a));
        // only samples blurred enough to reach this pixel: sharp nearer things don't bleed out
        float w = clamp(blur(q) * uMaxRadius - r + 1.0, 0.0, 1.0);
        sum += texture(uColour, q / size).rgb * w;
        weight += w;
    }
    return sum / weight;
}

vec3 bloom(vec2 uv) {
    const float weights[7] = float[7](0.12, 0.25, 0.45, 0.55, 0.5, 0.35, 0.2);
    vec3 sum = vec3(0.0);
    for (int level = 1; level <= 7; level++) {
        vec2 t = 0.75 / vec2(textureSize(uGlow, level));
        float l = float(level);
        vec3 c = textureLod(uGlow, uv + vec2(-t.x, -t.y), l).rgb + textureLod(uGlow, uv + vec2(t.x, -t.y), l).rgb
               + textureLod(uGlow, uv + vec2(-t.x, t.y), l).rgb + textureLod(uGlow, uv + vec2(t.x, t.y), l).rgb;
        sum += c * 0.25 * weights[level - 1];
    }
    return sum;
}

// horizontal streaks through the lights. The samples are a texel of the (quarter resolution) mip level apart,
// so the bilinear lookups overlap into a smooth streak rather than separate copies of the light
vec3 flare(vec2 uv, vec2 size) {
    const int TAPS = 24;
    vec3 streak = vec3(0.0);
    float step = 4.0 / size.x;
    for (int k = 1; k <= TAPS; k++) {
        float w = 1.0 - float(k) / float(TAPS + 1);
        streak += (textureLod(uGlow, uv + vec2(float(k) * step, 0.0), 2.0).rgb
                 + textureLod(uGlow, uv - vec2(float(k) * step, 0.0), 2.0).rgb) * w * w;
    }
    return streak * vec3(0.8, 0.9, 1.0) * 0.06;
}

// the ambient occlusion, upsampled with depth aware weights
float ambientOcclusion(vec2 p) {
    float z = linearDepth(texelFetch(uAODepth, ivec2(p), 0).r);
    // drawn after the snapshot, in front of what the occlusion was made from (a pedestrian, smoke): none
    if (linearDepth(texelFetch(uDepth, ivec2(p), 0).r) < z * 0.99 - 0.005) {
        return 1.0;
    }
    vec2 h = p * 0.5;
    float sum = 0.0;
    float weight = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 q = h + vec2(float(x), float(y));
            float zq = linearDepth(texelFetch(uAODepth, ivec2(q * 2.0), 0).r);
            float w = exp(-abs(zq - z) / max(0.02 * z, 0.01)) * (x == 0 && y == 0 ? 2.0 : 1.0);
            sum += texelFetch(uAO, ivec2(q), 0).r * w;
            weight += w;
        }
    }
    return sum / max(weight, 0.0001);
}

vec3 viewPosition(vec2 px) {
    float z = linearDepth(texelFetch(uDepth, ivec2(px), 0).r);
    vec2 ndc = (px - uView.xy) / uView.zw * 2.0 - 1.0;
    return vec3(ndc.x * z / uProj.x, ndc.y * z / uProj.y, -z);
}

vec2 toWindow(vec3 v) {
    return uView.xy + (vec2(v.x * uProj.x, v.y * uProj.y) / -v.z * 0.5 + 0.5) * uView.zw;
}

// The reflection of the ray from view space point P (window pixel p) in direction `ray`, by marching through
// the depth: the window position where it meets a surface, or x < 0 for none (it left the view, or met only
// the sky). Behind something in front (a car) it carries on, so reflections don't stop at its outline;
// `to_sky` tells whether, without meeting anything, it went only in front of the sky at the end (so it would
// show the sky), rather than behind anything or out of the view in front of something else
vec2 traceReflection(vec3 P, vec3 n, vec3 ray, vec2 p, out bool to_sky, out vec2 last) {
    bool hidden = false;
    bool over_sky = false;
    to_sky = false;
    last = vec2(-1.0);
    float dist = -P.z;
    // a little above the surface, so it doesn't hit itself; steps growing with the distance travelled
    vec3 q = P + n * (0.01 + 0.004 * dist);
    float step_len = 0.03 + 0.02 * dist;
    float jitter = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
    q += ray * step_len * jitter;
    vec3 prev = q;
    for (int i = 0; i < 28; i++) {
        prev = q;
        q += ray * step_len;
        step_len *= 1.15;
        if (q.z > -uNear) {
            break;
        }
        vec2 s = toWindow(q);
        if (s.x < uView.x || s.y < uView.y || s.x >= uView.x + uView.z || s.y >= uView.y + uView.w) {
            break;
        }
        float ds = texelFetch(uDepth, ivec2(s), 0).r;
        last = s;
        over_sky = ds >= 1.0;
        float zs = linearDepth(ds);
        float behind = -q.z - zs;
        hidden = hidden || behind > 0.0;
        if (behind > 0.0 && behind < step_len * 1.5 + 0.02 * zs) {
            // refine between the last two points
            vec3 a = prev;
            vec3 b = q;
            for (int k = 0; k < 4; k++) {
                vec3 m = (a + b) * 0.5;
                float zm = linearDepth(texelFetch(uDepth, ivec2(toWindow(m)), 0).r);
                if (-m.z > zm) {
                    b = m;
                } else {
                    a = m;
                }
            }
            return toWindow(b);
        }
    }
    to_sky = !hidden && over_sky;
    return vec2(-1.0);
}

// fade out towards the edges of the view, where the ray could as well have left it
float edgeFade(vec2 hit) {
    vec2 e = min(hit - uView.xy, uView.xy + uView.zw - hit) / (0.08 * uView.w);
    return clamp(min(e.x, e.y), 0.0, 1.0);
}

// the surface's normal at P (window pixel p), from the neighbouring depths (the smaller steps, so not across
// an edge), facing the viewer
vec3 depthNormal(vec2 p, vec3 P) {
    vec3 r = viewPosition(p + vec2(1.0, 0.0)) - P;
    vec3 l = P - viewPosition(p - vec2(1.0, 0.0));
    vec3 u = viewPosition(p + vec2(0.0, 1.0)) - P;
    vec3 d = P - viewPosition(p - vec2(0.0, 1.0));
    vec3 n = normalize(cross(abs(r.z) < abs(l.z) ? r : l, abs(u.z) < abs(d.z) ? u : d));
    return dot(n, P) > 0.0 ? -n : n;
}

// the surface's normal at window pixel p (view space point P), facing the viewer: as the scene wrote it, or else
// from the depths
vec3 viewNormal(vec2 p, vec3 P) {
    vec4 t = texelFetch(uNormal, ivec2(p), 0);
    vec3 n = t.xyz * 2.0 - 1.0;
    if (t.a > 0.3 && dot(n, n) > 0.25) {
        return normalize(n);
    }
    return depthNormal(p, P);
}

// whether the surface at window pixel p is a sprite (a flat card facing the camera, see brender.frag.glsl)
bool isSprite(vec2 p) {
    return texelFetch(uNormal, ivec2(p), 0).a > 0.85;
}

// how flat (facing up) the surface at P is
float flatGround(vec2 p, vec3 P) {
    return smoothstep(0.88, 0.97, abs(dot(viewNormal(p, P), uUp)));
}

float hash12(vec2 q) {
    vec3 q3 = fract(vec3(q.xyx) * 0.1031);
    q3 += dot(q3, q3.yzx + 33.33);
    return fract((q3.x + q3.y) * q3.z);
}

float valueNoise2(vec2 x) {
    vec2 i = floor(x);
    vec2 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), f.x), mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), f.x), f.y);
}

// Wet ground: the surfaces facing up reflect what glows (lights, neon, lit windows, car lights), smeared towards
// the viewer as on wet asphalt, and stronger at grazing angles. Not a mirror: how wet it is varies over the
// ground (in the world, so it stays put), puddles reflecting more sharply and strongly than the damp rest, and
// the rough surface breaks the reflection up
vec3 wetGround(vec2 p, vec2 size) {
    if (texelFetch(uDepth, ivec2(p), 0).r >= 1.0) {
        return vec3(0.0);
    }
    vec3 P = viewPosition(p);
    float flat_ground = flatGround(p, P);
    if (flat_ground <= 0.0) {
        return vec3(0.0);
    }
    vec3 v = normalize(P);
    bool to_sky;
    vec2 last;
    vec2 hit = traceReflection(P, uUp, reflect(v, uUp), p, to_sky, last);
    if (hit.x < 0.0) {
        return vec3(0.0);
    }
    // wetness: large patches (puddles a few metres across), with finer variation (the world unit is about 7m)
    vec2 w = (uToWorld[3] + P.x * uToWorld[0] + P.y * uToWorld[1] + P.z * uToWorld[2]).xz;
    float wet = smoothstep(0.4, 0.75, valueNoise2(w * 1.6) * 0.75 + valueNoise2(w * 7.0) * 0.25);
    float rough = 1.0 - wet;
    // the surface's grain jitters where the reflection is looked up, more where it is only damp
    vec2 grain = vec2(hash12(p), hash12(p + 17.3)) - 0.5;
    hit += grain * vec2(5.0, 9.0) * uScale * (0.4 + rough);
    vec3 sum = vec3(0.0);
    float stretch = (0.004 + 0.005 * rough) * uView.w;
    // and spread sideways a little, zigzagging, so the reflections of things side by side run together
    float spread = (0.003 + 0.004 * rough) * uView.w;
    float lod = mix(2.3, 3.6, rough);
    for (int k = -4; k <= 4; k++) {
        float weight = 1.0 - abs(float(k)) / 5.0;
        float side = (fract(float(k) * 0.618 + 0.31) - 0.5) * 2.0;
        sum += textureLod(uGlow, (hit + vec2(side * spread, float(k) * stretch)) / size, lod).rgb * weight;
    }
    sum /= 5.0;
    float grazing = 1.0 - max(dot(-v, uUp), 0.0);
    float fresnel = 0.15 + 0.85 * grazing * grazing * grazing;
    return sum * fresnel * flat_ground * edgeFade(hit) * uWet * (0.3 + 0.7 * wet);
}

// the ripples on water at view space point P: how they shift its mirror image, in window pixels
vec2 ripple(vec3 P) {
    // in the world, so they stay put as the camera moves
    vec2 w = (uToWorld[3] + P.x * uToWorld[0] + P.y * uToWorld[1] + P.z * uToWorld[2]).xz;
    float t = uTime;
    vec2 d = vec2(sin(dot(w, vec2(4.1, 1.9)) + t * 1.9) + 0.6 * sin(dot(w, vec2(-2.4, 6.3)) + t * 2.6)
                    + 0.35 * sin(dot(w, vec2(9.0, -7.0)) + t * 3.7),
                  cos(dot(w, vec2(-1.7, 4.7)) + t * 2.2) + 0.6 * cos(dot(w, vec2(6.8, 2.8)) + t * 2.9)
                    + 0.35 * cos(dot(w, vec2(-8.0, -10.0)) + t * 4.1));
    // smaller further away, and squashed vertically as the surface is seen at an angle
    float scale = 5.0 * uScale / (1.0 + 0.08 * -P.z);
    return d * scale * vec2(1.0, 0.6);
}

// world position of view space point P
vec3 toWorld(vec3 P) {
    return uToWorld[3] + P.x * uToWorld[0] + P.y * uToWorld[1] + P.z * uToWorld[2];
}

float shadowDepth(int level, ivec2 t) {
    if (level == 0) {
        return texelFetch(uShadowMap[0], t, 0).r;
    } else if (level == 1) {
        return texelFetch(uShadowMap[1], t, 0).r;
    }
    return texelFetch(uShadowMap[2], t, 0).r;
}

// view space point P in the map of `level`: its coordinates and depth (0..1)
vec3 shadowCoord(int level, vec3 P) {
    vec3 w = toWorld(P);
    vec3 lv = uShadowView[4 * level + 3] + w.x * uShadowView[4 * level] + w.y * uShadowView[4 * level + 1]
        + w.z * uShadowView[4 * level + 2];
    vec4 clip = uShadowProj[level] * vec4(lv, 1.0);
    return clip.xyz / clip.w * 0.5 + 0.5;
}

// a vector at least `least` long, in its direction
vec2 atLeast(vec2 v, float least) {
    float l = length(v);
    return l < least ? (l > 1e-6 ? v * (least / l) : vec2(least, 0.0)) : v;
}

// How much the point at map coordinates m (of `level`) is in the sun's shadow, 0..1: bilinear comparisons at a
// grid of taps over the pixel's footprint in the map (ax, ay: across the pixel, in texels; at least 2 texels, for
// soft edges), as many along each as it spans texels (2 to 6), so far off and at grazing angles thin shadows
// aren't stepped over (they flickered); each counting only what is more than `gap` (world units) nearer the sun
float shadowed(int level, vec3 m, vec2 ax, vec2 ay, float gap) {
    // (the map's depth is linear over its depth range: 2 / its projection's z scale)
    float threshold = m.z - gap * abs(uShadowProj[level][2][2]) * 0.5 - 0.00005;
    float texels = 1.0 / uShadowTexel[level];
    int last = int(texels) - 1;
    vec2 a = atLeast(ax, 2.0);
    vec2 b = atLeast(ay, 2.0);
    int nx = int(clamp(ceil(length(a)), 2.0, 6.0));
    int ny = int(clamp(ceil(length(b)), 2.0, 6.0));
    vec2 f = m.xy * texels - 0.5;
    float sum = 0.0;
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            vec2 g = f + a * ((float(i) + 0.5) / float(nx) - 0.5) + b * ((float(j) + 0.5) / float(ny) - 0.5);
            ivec2 t = ivec2(floor(g));
            vec2 fr = g - floor(g);
            float s00 = shadowDepth(level, clamp(t, ivec2(0), ivec2(last))) < threshold ? 1.0 : 0.0;
            float s10 = shadowDepth(level, clamp(t + ivec2(1, 0), ivec2(0), ivec2(last))) < threshold ? 1.0 : 0.0;
            float s01 = shadowDepth(level, clamp(t + ivec2(0, 1), ivec2(0), ivec2(last))) < threshold ? 1.0 : 0.0;
            float s11 = shadowDepth(level, clamp(t + ivec2(1, 1), ivec2(0), ivec2(last))) < threshold ? 1.0 : 0.0;
            sum += mix(mix(s00, s10, fr.x), mix(s01, s11, fr.x), fr.y);
        }
    }
    return sum / float(nx * ny);
}

// the view space direction through window pixel q
vec3 viewRay(vec2 q) {
    vec2 ndc = (q - uView.xy) / uView.zw * 2.0 - 1.0;
    return vec3(ndc.x / uProj.x, ndc.y / uProj.y, -1.0);
}

// How much view space point P (window pixel p, surface normal n, facing the sun by `facing`) is in the sun's
// shadow, 0..1, by the first of the maps that covers it (the nearer ones being sharper), blending into the next
// towards its edges. The filter covers the pixel's footprint on the surface (its neighbours' rays met with the
// surface's plane). Sprites are looked up just in front of their card, and only what is more than `gap` nearer
// the sun counts
float shadowLevels(vec2 p, vec3 P, vec3 n, float facing, float gap, bool sprite) {
    float dist = -P.z;
    vec3 rx = viewRay(p + vec2(1.0, 0.0));
    vec3 ry = viewRay(p + vec2(0.0, 1.0));
    float plane = dot(P, n);
    vec3 Px = rx * (plane / min(dot(rx, n), -1e-4));
    vec3 Py = ry * (plane / min(dot(ry, n), -1e-4));
    float result = 0.0;
    float remaining = 1.0;
    for (int level = 0; level < 3; level++) {
        if (uShadowTexel[level] <= 0.0) {
            continue;
        }
        float texels = 1.0 / uShadowTexel[level];
        // (a texel in world units: the map's width, 2 / its projection's x scale, over its texels)
        float texel = 2.0 / uShadowProj[level][0][0] * uShadowTexel[level];
        // a little off the surface, along its normal, more at grazing angles to the sun (against shadow acne)
        vec3 Q = sprite ? P + n * 0.01 : P + n * (1.5 * texel + 0.0003 * dist) * (1.0 + 2.0 * (1.0 - max(facing, 0.0)));
        vec3 m = shadowCoord(level, Q);
        if (m.x <= 0.0 || m.y <= 0.0 || m.x >= 1.0 || m.y >= 1.0 || m.z >= 1.0) {
            continue;
        }
        vec2 e = min(m.xy, 1.0 - m.xy) / 0.06;
        float fade = clamp(min(e.x, e.y), 0.0, 1.0);
        // the footprint: across the pixel, in this map's texels (at most 64 texels, against stray rays)
        vec3 m0 = shadowCoord(level, P);
        vec2 ax = (shadowCoord(level, Px).xy - m0.xy) * texels;
        vec2 ay = (shadowCoord(level, Py).xy - m0.xy) * texels;
        ax *= min(1.0, 64.0 / max(length(ax), 1e-6));
        ay *= min(1.0, 64.0 / max(length(ay), 1e-6));
        float s = shadowed(level, m, ax, ay, gap);
        result += remaining * fade * s;
        remaining *= 1.0 - fade;
        if (remaining <= 0.001) {
            break;
        }
    }
    return result;
}

// Sun shadows (the sun's shadow maps, of all the world): what the colour at p is multiplied by. What is in shadow
// ends up equally dark: the ground under a bridge, and the faces turned away from the sun (which get no sun
// either), those only darkened as far as the sun shading hasn't already. The sprites (pedestrians: flat cards
// facing the camera, marked with their normals) go by the maps alone, whichever way they face, not counting the
// slight overlap of their sun facing selves in the maps
float sunShadow(vec2 p, vec2 size) {
    if (uShadowTexel[0] + uShadowTexel[1] + uShadowTexel[2] <= 0.0 || texelFetch(uDepth, ivec2(p), 0).r >= 1.0) {
        return 1.0;
    }
    vec3 P = viewPosition(p);
    vec3 s = normalize(uSun.xyz);
    vec3 n = viewNormal(p, P);
    float facing = dot(n, s);
    // how bright what is in shadow is, relative to the ground in the sun
    float shade_level = 1.0 - min(0.38 * uSun.w, 0.8);
    if (isSprite(p)) {
        return mix(1.0, shade_level, shadowLevels(p, P, n, facing, 0.15, true));
    }
    // the sun shading the surface already has (see surfaceShading in brender.frag.glsl), which shadow only adds to
    float sun_shade = clamp(1.0 + uSunShading * 0.2 * (facing - dot(uUp, s)), 0.6, 1.04);
    float target = min(1.0, shade_level / sun_shade);
    // facing the sun: by the maps; facing away: in shadow
    float front = smoothstep(-0.02, 0.12, facing);
    float in_shadow = 1.0;
    if (front > 0.0) {
        in_shadow = mix(1.0, shadowLevels(p, P, n, facing, 0.0, false), front);
    }
    return mix(1.0, target, in_shadow);
}

// Ice and water (the mirroring surfaces, masked in the glow's alpha as 1 - 1 for ice, 1 - 0.5 for water): a soft
// mirror image of the scene, or of the sky where the reflection only meets that; more at grazing angles, and on
// water rippling
vec3 mirror(vec2 p, vec2 size, vec3 c) {
    float amount = 1.0 - textureLod(uGlow, p / size, 0.0).a;
    if (amount <= 0.01 || texelFetch(uDepth, ivec2(p), 0).r >= 1.0) {
        return c;
    }
    // (the kinds' masks blend at their edges, which then pass for another kind for a pixel)
    bool paint = amount < 0.375;
    bool water = !paint && amount < 0.75;
    amount = paint ? min(amount * 4.0, 1.0) * uPaint : water ? amount * 2.0 * uWater : amount * uIce;
    if (amount <= 0.0) {
        return c;
    }
    vec3 P = viewPosition(p);
    if (water) {
        // not on waterfalls and the sides of the liquid
        amount *= flatGround(p, P);
        if (amount <= 0.0) {
            return c;
        }
    }
    vec3 v = normalize(P);
    // ice and water lie flat; the car bodies go by their facets
    vec3 n = paint ? viewNormal(p, P) : uUp;
    if (dot(v, n) >= 0.0) {
        // seen from below (the camera under water)
        return c;
    }
    vec3 ray = reflect(v, n);
    bool to_sky;
    vec2 last;
    vec2 hit = traceReflection(P, n, ray, p, to_sky, last);
    float fade = 1.0;
    if (hit.x < 0.0 && paint) {
        // a gloss: what is that way, roughly (where the ray left the view)
        if (last.x < 0.0) {
            return c;
        }
        hit = last;
        fade = 0.7;
    } else if (hit.x < 0.0) {
        if (!to_sky) {
            // it went behind something (a car on the ice), or out of the view before meeting what it would show
            return c;
        }
        // the sky in that direction, if it shows there
        vec3 far_point = ray * 1000.0;
        hit = far_point.z < 0.0 ? toWindow(far_point) : vec2(-1.0);
        if (hit.x < uView.x || hit.y < uView.y || hit.x >= uView.x + uView.z || hit.y >= uView.y + uView.w
            || texelFetch(uDepth, ivec2(hit), 0).r < 1.0) {
            return c;
        }
    }
    if (water) {
        vec2 rippled = hit + ripple(P);
        // not where that shows something in front of the water (it would be a copy of what is above)
        if (rippled.x >= uView.x && rippled.y >= uView.y && rippled.x < uView.x + uView.z && rippled.y < uView.y + uView.w
            && linearDepth(texelFetch(uDepth, ivec2(rippled), 0).r) > -P.z) {
            hit = rippled;
        }
    }
    if (fade >= 1.0) {
        fade = edgeFade(hit);
    }
    // a little soft: ice isn't a perfect mirror, and paint is glossy
    vec2 t = (paint ? 8.0 : 3.0) * uScale / size;
    vec3 r = (texture(uColour, hit / size).rgb * 2.0 + texture(uColour, hit / size + vec2(t.x, 0.0)).rgb
            + texture(uColour, hit / size - vec2(t.x, 0.0)).rgb + texture(uColour, hit / size + vec2(0.0, t.y)).rgb
            + texture(uColour, hit / size - vec2(0.0, t.y)).rgb) / 6.0;
    float grazing = 1.0 - max(dot(-v, n), 0.0);
    float fresnel = paint ? 0.1 + 0.6 * grazing * grazing * grazing
        : water ? 0.18 + 0.42 * grazing * grazing * grazing : 0.12 + 0.5 * grazing * grazing * grazing;
    float f = clamp(amount * fresnel * fade, 0.0, 1.0);
    if (paint) {
        // a clear coat over the paint: its reflection adds to the colour underneath, which shows through
        return c * (1.0 - 0.5 * f) + r * f;
    }
    return mix(c, r, f);
}

// hot air rising above fires: a wavy offset in a plume above each
vec2 shimmer(vec2 p) {
    vec2 offset = vec2(0.0);
    for (int i = 0; i < uShimmerCount; i++) {
        vec3 v = uShimmers[i].xyz;
        if (v.z > -0.1) {
            continue;
        }
        vec2 c = uView.xy + (vec2(v.x * uProj.x, v.y * uProj.y) / -v.z * 0.5 + 0.5) * uView.zw;
        float radius = 0.35 * uProj.y * 0.5 * uView.w / -v.z; // the plume's width, in pixels
        vec2 d = (p - c) / radius;
        float plume = smoothstep(1.0, 0.3, abs(d.x)) * smoothstep(-0.4, 0.2, d.y) * smoothstep(3.0, 1.0, d.y);
        if (plume <= 0.0) {
            continue;
        }
        float amp = 0.65 * uScale * uShimmers[i].w * plume;
        offset += amp * vec2(sin(p.y * 0.09 / uScale - uTime * 9.0 + float(i)), 0.5 * cos(p.x * 0.07 / uScale + uTime * 7.0));
    }
    return offset;
}

vec3 grade(vec3 c) {
    // a gentle S curve and a little more saturation; cooler at night
    vec3 s = c * c * (3.0 - 2.0 * c);
    c = mix(c, s, 0.18 * uGrade.x);
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, 1.0 + 0.12 * uGrade.x);
    c *= mix(vec3(1.0), vec3(0.9, 0.97, 1.12), uGrade.x * uGrade.y);
    return clamp(c, 0.0, 1.0);
}

void main() {
    vec2 size = vec2(textureSize(uColour, 0));
    vec2 p = gl_FragCoord.xy;
    if (uShimmerCount > 0) {
        p = clamp(p + shimmer(p), vec2(0.5), size - 0.5);
    }
    vec3 c = dof(p, size);
    if (uAOStrength > 0.0) {
        float ao = ambientOcclusion(p);
        // light sources aren't occluded
        float lit = uGlowStrength > 0.0 ? clamp(dot(textureLod(uGlow, p / size, 0.0).rgb, vec3(4.0)), 0.0, 1.0) : 0.0;
        // up to 1 blends the occlusion in, above that it deepens (ao^strength)
        float shade = uAOStrength <= 1.0 ? mix(1.0, ao, uAOStrength) : pow(ao, uAOStrength);
        c *= mix(1.0, shade, 1.0 - lit);
    }
    if (uSun.w > 0.0) {
        // (light sources aren't shadowed)
        float lit = uGlowStrength > 0.0 ? clamp(dot(textureLod(uGlow, p / size, 0.0).rgb, vec3(4.0)), 0.0, 1.0) : 0.0;
        c *= mix(1.0, sunShadow(p, size), 1.0 - lit);
    }
    if (uIce > 0.0 || uWater > 0.0 || uPaint > 0.0) {
        c = mirror(p, size, c);
    }
    if (uWet > 0.0) {
        c += wetGround(p, size);
    }
    if (uSpeed > 0.0) {
        // radial blur towards the edges of the view
        vec2 centre = uView.xy + 0.5 * uView.zw;
        vec2 dir = p - centre;
        float r = length(dir) / (0.5 * uView.w);
        float amount = uSpeed * smoothstep(0.35, 1.2, r);
        if (amount > 0.0) {
            // closely spaced samples, offset per pixel, so bright lights smear instead of leaving copies
            float jitter = fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
            vec3 sum = c;
            for (int k = 1; k <= 12; k++) {
                sum += texture(uColour, (p - dir * (0.0045 * (float(k) - jitter) * amount)) / size).rgb;
            }
            c = mix(c, sum / 13.0, clamp(amount * 1.5, 0.0, 1.0));
        }
    }
    if (uGlowStrength > 0.0) {
        vec2 uv = p / size;
        c += bloom(uv) * uGlowStrength;
        if (uFlareStrength > 0.0) {
            c += flare(uv, size) * uFlareStrength;
        }
    }
    if (uGrade.x > 0.0) {
        c = grade(c);
    }
    fragColour = vec4(c, 1.0);
}
