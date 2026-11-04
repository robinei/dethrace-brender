##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
##endif

// Weather (see post.c): snowflakes or raindrops in a box of the world around the camera. Each is placed by its
// number, falls (and sways) with time and wraps around in the box, so they stay put in the world as the
// camera moves through them. Six vertices each: a little square for a flake, a thin quad along its fall for a
// drop (as the eye smears it)

uniform vec3 uToWorld[4]; // view to world: the x, y and z axes, the position
uniform vec4 uView;       // the view in window pixels
uniform vec2 uProj;       // projection x and y scale
uniform vec2 uSize;       // the window, in pixels
uniform float uTime;
uniform float uScale;     // window height / 1080
uniform float uRain;      // 1 for rain, 0 for snow
uniform float uBox;       // the box's size
uniform float uInner;     // the box of the layer inside this one (left out of this), 0 for none
uniform float uMinPixels; // the least size drawn, in pixels at 1080p (smaller ones are drawn that big, fainter)

out vec2 vCorner;  // -1..1 across the particle
out float vDepth;  // its distance along the view
out float vFade;
out float vSub; // how much of the drawn size the particle really covers (fainter where drawn bigger)

float hash(float n) {
    return fract(sin(n) * 43758.5453123);
}

vec3 toView(vec3 w) {
    vec3 d = w - uToWorld[3];
    return vec3(dot(d, uToWorld[0]), dot(d, uToWorld[1]), dot(d, uToWorld[2]));
}

vec2 toNdc(vec3 v) {
    vec2 window = uView.xy + (vec2(v.x * uProj.x, v.y * uProj.y) / -v.z * 0.5 + 0.5) * uView.zw;
    return window / uSize * 2.0 - 1.0;
}

void main() {
    int id = gl_VertexID / 6;
    int corner = gl_VertexID - id * 6;
    vec2 c = vec2(corner == 1 || corner == 2 || corner == 4 ? 1.0 : -1.0, corner == 2 || corner == 4 || corner == 5 ? 1.0 : -1.0);
    float n = float(id);
    vec3 seed = vec3(hash(n * 1.37 + 0.1), hash(n * 2.71 + 0.3), hash(n * 3.17 + 0.7));
    // falling: snow slowly, swaying; rain fast, slanting a little (world units: about 7m)
    float fall = mix(0.12 + 0.06 * seed.x, 1.3 + 0.3 * seed.x, uRain);
    vec3 drift = vec3(0.0, -fall * uTime, 0.0);
    drift.xz += mix(0.05 * vec2(sin(uTime * 0.9 + n), cos(uTime * 0.7 + n * 1.3)), vec2(0.08, 0.03) * uTime, uRain);
    vec3 cam = uToWorld[3];
    vec3 rel = mod(seed * uBox + drift - cam + 0.5 * uBox, uBox) - 0.5 * uBox;
    vec3 w = cam + rel;
    vec3 v = toView(w);
    vFade = 0.0;
    vDepth = -v.z;
    vCorner = c;
    // fading in at the box's far side and in front of the camera
    float dist = length(rel);
    // (raindrops not right in front of the camera, where they would streak across the view)
    vFade = (1.0 - smoothstep(0.3 * uBox, 0.5 * uBox, dist)) * smoothstep(0.05, 0.15, -v.z) * mix(1.0, smoothstep(0.2, 0.45, -v.z), uRain);
    // (fading in where the layer inside fades out)
    if (uInner > 0.0) {
        vFade *= smoothstep(0.3 * uInner, 0.5 * uInner, dist);
    }
    vSub = 1.0;
    if (v.z > -0.05) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    vec2 px = 2.0 / uSize; // a window pixel in ndc
    if (uRain > 0.5) {
        // from where it is to where it was a moment ago, a pixel or so wide
        vec2 a = toNdc(v);
        vec3 tail = toView(w + vec3(-0.08, fall, -0.03) * 0.045);
        vec2 b = tail.z < -0.05 ? toNdc(tail) : a + vec2(0.0, 0.02);
        vec2 along = b - a;
        if (dot(along / px, along / px) < 1.0) {
            along = vec2(0.0, px.y);
        }
        // at most a tenth of the view's height
        float most = 0.1 * uView.w * px.y;
        along *= min(1.0, most / max(length(along), 1e-6));
        // about 1cm wide, at least the least size
        float width = 0.0015 * uProj.y * 0.5 * uView.w / -v.z;
        float least = 0.4 * uMinPixels * max(uScale, 0.5);
        vSub = min(1.0, width / least);
        vec2 across = normalize(vec2(-along.y, along.x) / px) * px * max(width, least);
        gl_Position = vec4(a + along * (c.y * 0.5 + 0.5) + across * c.x, 0.0, 1.0);
    } else {
        // about 3cm across, at least the least size
        float size = 0.005 * uProj.y * 0.5 * uView.w / -v.z;
        float least = uMinPixels * uScale;
        vSub = min(1.0, size / least);
        float r = max(size, least);
        gl_Position = vec4(toNdc(v) + c * r * px, 0.0, 1.0);
    }
}
