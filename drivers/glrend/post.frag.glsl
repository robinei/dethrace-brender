##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
##endif

// Post-processing of a view (see post.c), in this order: heat shimmer, depth of field, ambient occlusion,
// speed blur, glow (bloom, light streaks and a lens ghost), colour grading
uniform sampler2D uColour;
uniform sampler2D uDepth;
uniform sampler2D uGlow;
uniform sampler2D uAO;
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

// horizontal streaks through the lights, and a faint ghost mirrored through the centre of the view
vec3 flare(vec2 uv, vec2 size) {
    vec3 streak = vec3(0.0);
    float step = 22.0 * uScale / size.x;
    for (int k = 1; k <= 8; k++) {
        float w = 1.0 - float(k) / 9.0;
        streak += (textureLod(uGlow, uv + vec2(float(k) * step, 0.0), 2.0).rgb
                 + textureLod(uGlow, uv - vec2(float(k) * step, 0.0), 2.0).rgb) * w * w;
    }
    vec2 centre = (uView.xy + 0.5 * uView.zw) / size;
    vec3 ghost = textureLod(uGlow, 2.0 * centre - uv, 5.0).rgb;
    return streak * vec3(0.8, 0.9, 1.0) * 0.18 + ghost * vec3(0.6, 0.8, 1.0) * 0.25;
}

// the ambient occlusion, upsampled with depth aware weights
float ambientOcclusion(vec2 p) {
    float z = linearDepth(texelFetch(uDepth, ivec2(p), 0).r);
    vec2 h = p * 0.5;
    float sum = 0.0;
    float weight = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 q = h + vec2(float(x), float(y));
            float zq = linearDepth(texelFetch(uDepth, ivec2(q * 2.0), 0).r);
            float w = exp(-abs(zq - z) / max(0.02 * z, 0.01)) * (x == 0 && y == 0 ? 2.0 : 1.0);
            sum += texelFetch(uAO, ivec2(q), 0).r * w;
            weight += w;
        }
    }
    return sum / max(weight, 0.0001);
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
        float amp = 2.5 * uScale * uShimmers[i].w * plume;
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
        c *= mix(1.0, ao, uAOStrength * (1.0 - lit));
    }
    if (uSpeed > 0.0) {
        // radial blur towards the edges of the view
        vec2 centre = uView.xy + 0.5 * uView.zw;
        vec2 dir = p - centre;
        float r = length(dir) / (0.5 * uView.w);
        float amount = uSpeed * smoothstep(0.35, 1.2, r);
        if (amount > 0.0) {
            vec3 sum = c;
            for (int k = 1; k <= 6; k++) {
                sum += texture(uColour, (p - dir * (0.009 * float(k) * amount)) / size).rgb;
            }
            c = mix(c, sum / 7.0, clamp(amount * 1.5, 0.0, 1.0));
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
