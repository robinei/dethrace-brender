##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
##endif

// Weather particles (see weather.vert.glsl), hidden behind the scene by its depth
uniform sampler2D uDepth;
uniform float uNear;
uniform float uFar;
uniform float uRain;
uniform float uBrightness; // of the scene (dark at night)
uniform float uAmount;

in vec2 vCorner;
in float vDepth;
in float vFade;

out vec4 fragColour;

float linearDepth(float d) {
    return 2.0 * uNear * uFar / (uFar + uNear - (2.0 * d - 1.0) * (uFar - uNear));
}

void main() {
    float scene = linearDepth(texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r);
    // a little soft where it meets what is behind
    float visible = clamp((scene - vDepth) / 0.03, 0.0, 1.0);
    float shape = uRain > 0.5 ? 1.0 - abs(vCorner.x) : 1.0 - smoothstep(0.3, 1.0, length(vCorner));
    float a = shape * visible * vFade * (uRain > 0.5 ? 0.3 : 0.8) * min(uAmount, 1.0);
    if (a <= 0.003) {
        discard;
    }
    vec3 colour = uRain > 0.5 ? vec3(0.75, 0.8, 0.85) : vec3(0.95, 0.97, 1.0);
    fragColour = vec4(colour * uBrightness, a);
}
