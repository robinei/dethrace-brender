##ifdef GL_ES
#version 300 es
precision highp float;
##endif
##ifdef GL_CORE
#version 140
#extension GL_ARB_explicit_attrib_location:require
##endif

// The value noise's lattice (brender.frag.glsl's valueNoise, see make_noise_texture in shader_brender.c): the hash at each of
// its integer points, a layer of it at a time (uLayer: z; x and y: the pixel)
uniform int uLayer;

layout (location = 0) out vec4 fragColour;

float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

void main() {
    fragColour = vec4(hash3(vec3(floor(gl_FragCoord.xy), float(uLayer))), 0.0, 0.0, 1.0);
}
