##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
##endif

// Ambient occlusion at half resolution, from the depth of the view (in the manner of McGuire et al.,
// "Scalable Ambient Obscurance"). View space positions are rebuilt from depth with the view's projection; normals
// from the neighbouring depths (the side with the smaller step, so edges don't bend them). Points of the
// scene on a disc around each pixel occlude it by how much they rise above its surface, more the closer
// they are, and not at all beyond the radius: so it collects in creases, corners and under things, flat
// open ground stays clear, and nothing far in front (a pedestrian before a wall) leaves a halo.
// The disc is rotated in a 4x4 tiled pattern, which the two depth aware blur passes after it remove.
//   uPass 0: ambient occlusion, 1 and 2: blur uAO horizontally / vertically
uniform sampler2D uDepth;
uniform sampler2D uAO;
uniform int uPass;
uniform vec4 uView;     // the 3d view in window pixels: x, y, width, height
uniform vec2 uProj;     // projection x and y scale (cot(fov / 2) / aspect, cot(fov / 2))
uniform float uNear;
uniform float uFar;
uniform float uRadius;  // world units

out vec4 fragColour;

const int SAMPLES = 16;
const float TURNS = 7.0; // spiral turns over the samples (coprime with SAMPLES)

float linearDepth(float d) {
    return 2.0 * uNear * uFar / (uFar + uNear - (2.0 * d - 1.0) * (uFar - uNear));
}

vec3 viewPosition(vec2 px) {
    float z = linearDepth(texelFetch(uDepth, ivec2(px), 0).r);
    vec2 ndc = (px - uView.xy) / uView.zw * 2.0 - 1.0;
    return vec3(ndc.x * z / uProj.x, ndc.y * z / uProj.y, -z);
}

bool outside(vec2 px) {
    return px.x < uView.x || px.y < uView.y || px.x >= uView.x + uView.z || px.y >= uView.y + uView.w;
}

// a depth aware blur of the half resolution AO along one axis
float blur(ivec2 q, ivec2 dir) {
    const float weights[5] = float[5](0.20, 0.17, 0.12, 0.07, 0.03);
    ivec2 size = textureSize(uAO, 0);
    float z = linearDepth(texelFetch(uDepth, q * 2 + 1, 0).r);
    float sum = texelFetch(uAO, q, 0).r * weights[0];
    float weight = weights[0];
    for (int i = 1; i < 5; i++) {
        for (int side = -1; side <= 1; side += 2) {
            ivec2 t = clamp(q + dir * (i * side), ivec2(0), size - 1);
            float zt = linearDepth(texelFetch(uDepth, t * 2 + 1, 0).r);
            float w = weights[i] * max(0.0, 1.0 - abs(zt - z) / (0.03 * z + 0.02));
            sum += texelFetch(uAO, t, 0).r * w;
            weight += w;
        }
    }
    return sum / weight;
}

void main() {
    if (uPass != 0) {
        fragColour = vec4(blur(ivec2(gl_FragCoord.xy), uPass == 1 ? ivec2(1, 0) : ivec2(0, 1)));
        return;
    }
    vec2 p = floor(gl_FragCoord.xy) * 2.0 + 1.0;
    if (outside(p) || texelFetch(uDepth, ivec2(p), 0).r >= 1.0) {
        fragColour = vec4(1.0);
        return;
    }
    vec3 P = viewPosition(p);
    vec3 r = viewPosition(p + vec2(2.0, 0.0)) - P;
    vec3 l = P - viewPosition(p - vec2(2.0, 0.0));
    vec3 u = viewPosition(p + vec2(0.0, 2.0)) - P;
    vec3 d = P - viewPosition(p - vec2(0.0, 2.0));
    vec3 dx = abs(r.z) < abs(l.z) ? r : l;
    vec3 dy = abs(u.z) < abs(d.z) ? u : d;
    vec3 N = normalize(cross(dx, dy));
    if (dot(N, -P) < 0.0) {
        N = -N;
    }
    // the radius on screen, in window pixels (kept within a tenth of the view, for nearby surfaces)
    float disc = min(uRadius * uProj.y * 0.5 * uView.w / -P.z, 0.1 * uView.w);
    if (disc < 1.0) {
        fragColour = vec4(1.0);
        return;
    }
    // 4x4 tiled rotation (a Bayer matrix), which the 4x4-or-wider blur smooths out exactly
    ivec2 cell = ivec2(gl_FragCoord.xy) & 3;
    int bayer = ((cell.x ^ cell.y) & 1) * 8 + (cell.y & 1) * 4 + (((cell.x ^ cell.y) >> 1) & 1) * 2 + ((cell.y >> 1) & 1);
    float angle = 6.2831853 * (float(bayer) + 0.5) / 16.0;
    float radius2 = uRadius * uRadius;
    float occlusion = 0.0;
    for (int i = 0; i < SAMPLES; i++) {
        float f = (float(i) + 0.5) / float(SAMPLES);
        float a = angle + f * TURNS * 6.2831853;
        vec2 q = p + vec2(cos(a), sin(a)) * disc * f;
        if (outside(q)) {
            continue;
        }
        vec3 v = viewPosition(q) - P;
        float vv = dot(v, v);
        float len = sqrt(vv);
        // how far the point rises above the surface (the cosine to the normal, less a bias so the gentle
        // folds of faceted models don't count), more the nearer it is, fading out to nothing at the radius.
        // Tiny steps don't count either: the car shadows are drawn just in front of the road they darken
        float rise = max((dot(v, N) - 0.04 * uRadius) / max(len, 1e-5) - 0.25, 0.0) / 0.75;
        float near = uRadius / (len + 0.25 * uRadius);
        float falloff = max(1.0 - vv / radius2, 0.0);
        occlusion += rise * near * falloff * falloff;
    }
    // about half the disc rising at 45 degrees (a corner) darkens it by about two thirds
    occlusion *= 3.0 / float(SAMPLES);
    // fading out in the distance, where it would only be noise: where the depth buffer's steps (24 bits, growing
    // with the distance squared) come near the bias, or the disc is only a few pixels across
    float z = -P.z;
    float depth_step = z * z * (uFar - uNear) / (uNear * uFar * 16777216.0);
    occlusion *= (1.0 - smoothstep(0.004 * uRadius, 0.015 * uRadius, depth_step)) * smoothstep(3.0, 8.0, disc);
    fragColour = vec4(max(0.0, 1.0 - occlusion));
}
