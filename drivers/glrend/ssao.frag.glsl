##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
##endif

// Ambient occlusion at half resolution, from the depth of the view. View space positions are rebuilt
// from depth with the view's projection; normals from the neighbouring depths (the side with the smaller
// step, so edges don't bend them). Samples in a hemisphere around the normal count as occluded when the
// scene at their position is in front of them, within the radius.
uniform sampler2D uDepth;
uniform vec4 uView;     // the 3d view in window pixels: x, y, width, height
uniform vec2 uProj;     // projection x and y scale (cot(fov / 2) / aspect, cot(fov / 2))
uniform float uNear;
uniform float uFar;
uniform float uRadius;  // world units

out vec4 fragColour;

const int SAMPLES = 12;

float linearDepth(float d) {
    return 2.0 * uNear * uFar / (uFar + uNear - (2.0 * d - 1.0) * (uFar - uNear));
}

vec3 viewPosition(vec2 px) {
    float z = linearDepth(texelFetch(uDepth, ivec2(px), 0).r);
    vec2 ndc = (px - uView.xy) / uView.zw * 2.0 - 1.0;
    return vec3(ndc.x * z / uProj.x, ndc.y * z / uProj.y, -z);
}

vec2 toPixel(vec3 v) {
    vec2 ndc = vec2(v.x * uProj.x, v.y * uProj.y) / -v.z;
    return uView.xy + (ndc * 0.5 + 0.5) * uView.zw;
}

void main() {
    vec2 p = floor(gl_FragCoord.xy) * 2.0 + 1.0;
    if (p.x < uView.x || p.y < uView.y || p.x >= uView.x + uView.z || p.y >= uView.y + uView.w
        || texelFetch(uDepth, ivec2(p), 0).r >= 1.0) {
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
    // rotate the sample pattern per pixel (interleaved gradient noise), the blur hides the noise
    float angle = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    vec3 helper = abs(N.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 T = normalize(cross(helper, N));
    vec3 B = cross(N, T);
    float occlusion = 0.0;
    for (int i = 0; i < SAMPLES; i++) {
        float f = (float(i) + 0.5) / float(SAMPLES);
        float a = angle + float(i) * 2.39996323;
        float s = sqrt(1.0 - f);
        vec3 dir = vec3(cos(a) * s, sin(a) * s, sqrt(f)); // cosine weighted hemisphere
        float scale = mix(0.2, 1.0, f * f);
        vec3 S = P + (T * dir.x + B * dir.y + N * dir.z) * uRadius * scale;
        vec2 q = toPixel(S);
        if (q.x < uView.x || q.y < uView.y || q.x >= uView.x + uView.z || q.y >= uView.y + uView.w) {
            continue;
        }
        float scene = linearDepth(texelFetch(uDepth, ivec2(q), 0).r);
        float in_front = step(scene, -S.z - 0.02 * uRadius);
        float in_range = smoothstep(0.0, 1.0, uRadius / max(abs(-P.z - scene), 0.0001));
        occlusion += in_front * in_range;
    }
    fragColour = vec4(1.0 - occlusion / float(SAMPLES));
}
