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

#define MAX_LIGHTS                   48 /* Must match up with BRender */
#define MAX_CLIP_PLANES              6

#define ENABLE_GAMMA_CORRECTION      0
#define ENABLE_SIMULATE_8BIT_COLOUR  0
#define ENABLE_SIMULATE_16BIT_COLOUR 0

#define UV_SOURCE_MODEL              0
#define UV_SOURCE_ENV_L              1
#define UV_SOURCE_ENV_I              2

// 3dfx driver uses a fog table. This approximates the behavior :/
#define FOGGING_EMULATE_3DFX_DENSITY_MULTIPLIER 80

struct br_light
{
    vec4 position;    /* (X, Y, Z, 1) */
    vec4 direction;   /* (X, Y, Z, 0), normalised */
    vec4 half_;       /* (X, Y, Z, 0), normalised */
    vec4 colour;      /* (R, G, B, 0), normalised */
    vec4 iclq;        /* (intensity, constant, linear, attenutation) */
    vec2 spot_angles; /* (inner, outer), if (0.0, 0.0), then this is a point light. */
};

layout(std140) uniform br_scene_state
{
    vec4 eye_view; /* Eye position in view-space */
    br_light lights[MAX_LIGHTS];
    uint num_lights;

    vec4 clip_planes[MAX_CLIP_PLANES];
    uint num_clip_planes;

    float hither_z;
    float yon_z;
};

layout(std140) uniform br_model_state
{
    mat4 model_view;
    mat4 projection;
    mat4 projection_brender;
    mat4 mvp;
    mat4 normal_matrix;
    mat4 environment;
    mat4 map_transform;
    vec4 surface_colour;
    vec4 clear_colour;
    vec4 eye_m; /* Eye position in model-space */

    float ka; /* Ambient mod */
    float ks; /* Specular mod (doesn't seem to be used by Croc) */
    float kd; /* Diffuse mod */
    float power;
    uint lighting; /* Is this surface lit? */
    int uv_source;
    bool disable_colour_key;
    bool disable_texture;
    bool fog_enabled;
    vec3 fog_colour;
    float fog_min;
    float fog_max;
    float alpha;
    uint prelit;
};

in vec4 position;
in vec2 uv;
in vec4 normal;
in vec4 colour;

in vec3 rawPosition;
in vec3 rawNormal;

in vec3 v_frag_pos;

layout (location = 0) out vec4 fragColour;
// Light sources, for the post-processed bloom (only stored while the view is post-processed)
layout (location = 1) out vec4 glowColour;

uniform sampler2D main_texture;
uniform bool alpha_to_coverage;
// Glow mask of the texture (light source texels) and its strength; 0 for none
uniform sampler2D glow_texture;
uniform float glow_intensity;
// Night: the scene is darkened to scene_brightness, except for light source texels, and lit by spot lights
// (car headlights): position + range, then direction + strength, in view space
#define MAX_SPOTS 32
uniform float scene_brightness;
uniform int num_spots;
uniform vec4 spots[2 * MAX_SPOTS];
// how much the spots light this fragment, also lifts the darkness depth cue
float spotLight = 0.0;

float spotLighting(vec3 p, vec3 n) {
    float sum = 0.0;
    for (int i = 0; i < num_spots; i++) {
        vec3 to_light = spots[2 * i].xyz - p;
        float d = length(to_light);
        float range = spots[2 * i].w;
        if (d >= range) {
            continue;
        }
        vec3 l = to_light / max(d, 0.0001);
        float cone = smoothstep(0.82, 0.95, dot(-l, spots[2 * i + 1].xyz));
        float falloff = 1.0 - d / range;
        float facing = dot(n, n) > 0.01 ? max(dot(n, l), 0.0) * 0.8 + 0.2 : 1.0;
        sum += cone * falloff * falloff * facing * spots[2 * i + 1].w;
    }
    return sum;
}
// Fog by distance from the eye, from fog_min to fully fogged at fog_max
uniform bool fog_distance;
// Above 0: fade out (alpha) from this fraction of yon_z to yon_z, by distance from the eye
uniform float distance_fade;
uniform bool sharp_filter;

vec3 adjustBrightness(in vec3 colour, in float brightness)
{
    return colour + brightness;
}

/* use values between -1 and 1 */
vec3 adjustContrast(in vec3 colour, in float contrast)
{
    return 0.5 + (1.0 + contrast) * (colour - 0.5);
}

/* use values between -1 and 1 */
vec3 adjustExposure(in vec3 colour, in float exposure)
{
    // return (1.0 + exposure) * colour;
    return vec3(1.0) - exp(-colour * exposure);
}

vec3 adjustSaturation(in vec3 colour, in float saturation)
{
    /* https://www.w3.org/TR/WCAG21/#dfn-relative-luminance */
    const vec3 luminosityFactor = vec3(0.2126, 0.7152, 0.0722);
    vec3 grayscale = vec3(dot(colour, luminosityFactor));

    return mix(grayscale, colour, 1.0 + saturation);
}


vec2 SurfaceMapEnvironment(in vec3 eye, in vec3 normal, in mat4 model_to_environment) {
    vec3 r;
    vec4 wr;
    float d, cu, cv;

    /*
     * Generate reflected vector
     */
    r = reflect(eye, normalize(normal));

    /*
     * Rotate vector into the environment frame.
     * This should be the identity matrix if no environment is set.
     */
    wr = model_to_environment * vec4(r, 0.0);
    vec3 wr2 = normalize(wr.xyz);

    /*
     * Convert vector to environment coordinates
     */
    cu = 0.5 + atan(wr2.x, -wr2.z) * 0.159154943091895; /* 1/(2*PI) */
    cv = 0.5 + -wr2.y * 0.5;

    return vec2(cu, cv);
}

vec2 SurfaceMap(in vec3 position, in vec3 normal, in vec2 uv)
{
    if(uv_source == UV_SOURCE_ENV_L) {
        /*
         * Generate U,V for environment assuming local eye.
         *
         * softrend/mapping.c - SurfaceMapEnvironmentLocal()
         */
        vec3 eye = normalize(eye_m.xyz - position);
        uv = SurfaceMapEnvironment(eye, normal, environment);
    } else if(uv_source == UV_SOURCE_ENV_I) {
        /*
         * Generate U,V for environment assuming infinite eye.
         *
         * softrend/mapping.c - SurfaceMapEnvironmentInfinite()
         */
        vec3 eye = normalize(eye_m.xyz);
        uv = SurfaceMapEnvironment(eye, normal, environment);
    } else {
        /* nop */
    }

    /* Apply the map transformation. */
    return (map_transform * vec4(uv, 1.0, 0.0)).xy;
}


void processClipPlanes() {
    for(uint i = 0u; i < num_clip_planes; i++) {
        // calculate signed plane-vertex distance
        float d = dot(clip_planes[i], projection_brender * model_view * vec4(rawPosition.xyz, 1));
        if (d < 0.0) {
            discard;
        }
    }
}

// Applies the fog, returns its strength
float doDistanceFog() {
    if (fog_enabled && fog_distance) {
        // distance rather than depth: the fog does not change as the view turns, and fully fogs geometry
        // before the yon plane clips it, also at the sides of a wide view
        float d = length(position.xyz);
        float t = clamp((d - fog_min) / max(fog_max - fog_min, 0.0001), 0.0, 1.0);
        // dense early like the original (about half at a quarter of the way), then easing into full fog
        float f = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
        if (fog_colour == vec3(0.0)) {
            // headlights reach into the darkness
            f *= 1.0 - clamp(spotLight, 0.0, 1.0);
        }
        fragColour.rgb = mix(fragColour.rgb, fog_colour * scene_brightness, f);
        return f;
    }
    if (fog_enabled) {

        // Compute exponential fog factor
        float linear_depth = (hither_z * yon_z / (yon_z - gl_FragCoord.z * (yon_z - hither_z)));

        if (linear_depth < fog_min) {
            //return;
        }

        float linear_depth_normalized = linear_depth / yon_z;
        float density = 1/fog_max * FOGGING_EMULATE_3DFX_DENSITY_MULTIPLIER;
        float fogging_factor = 1.0 - exp(-density * linear_depth_normalized * linear_depth_normalized);
        fragColour.rgb = mix(fragColour.rgb, fog_colour, clamp(fogging_factor, 0.0, 1.0));
        return clamp(fogging_factor, 0.0, 1.0);
    }
    return 0.0;
}

void main() {
    vec4 textureColour;
    float coverage = 1.0;

    processClipPlanes();

    vec2 mappedUV = SurfaceMap(rawPosition, rawNormal, uv);

    if (disable_texture) {
        textureColour = surface_colour;
    } else {
        if (sharp_filter) {
            // Anti-aliased nearest: flat texels, with a bilinear blend only in a ~1 screen pixel band
            // around texel borders. When minified the band covers the whole texel (plain trilinear).
            // textureGrad keeps the mip level and anisotropy based on the unsnapped coordinates.
            vec2 size = vec2(textureSize(main_texture, 0));
            vec2 texel = mappedUV * size;
            vec2 seam = floor(texel + 0.5);
            vec2 band = max(fwidth(texel), vec2(1e-5));
            vec2 snapped = (seam + clamp((texel - seam) / band, -0.5, 0.5)) / size;
            textureColour = textureGrad(main_texture, snapped, dFdx(mappedUV), dFdy(mappedUV));
        } else {
            textureColour = texture(main_texture, mappedUV);
        }
        // full black pixel is transparent. Paletted textures also store the key as
        // alpha == 0, so filtered/mipmapped texels carry partial coverage: discard below
        // half coverage and un-premultiply the rest to avoid dark fringes.
        if(!disable_colour_key) {
            if (textureColour.a <= 0.0 || textureColour.rgb == vec3(0)) {
                discard;
            }
            if (alpha_to_coverage) {
                // Multisampled: turn alpha into sample coverage, sharpened to a ~1 pixel wide
                // edge so the cut-out stays crisp while thin distant details fade smoothly.
                coverage = clamp((textureColour.a - 0.5) / max(fwidth(textureColour.a), 0.0001) + 0.5, 0.0, 1.0);
                if (coverage <= 0.0) {
                    discard;
                }
            } else if (textureColour.a < 0.5) {
                discard;
            }
            textureColour.rgb /= textureColour.a;
        }
    }

    if (lighting == 1u) {
        fragColour.rgb = colour.rgb * textureColour.rgb;
    } else {
        fragColour.rgb = textureColour.rgb;
    }

    float glowMask = 0.0;
    if (glow_intensity > 0.0 && !disable_texture) {
        glowMask = texture(glow_texture, mappedUV).r * glow_intensity;
    }
    if (scene_brightness < 1.0 || num_spots > 0) {
        // night: dark, lit by the headlights (warm white), light sources keep their own brightness
        if (num_spots > 0) {
            spotLight = spotLighting(position.xyz, normalize(normal.xyz));
        }
        vec3 lit = fragColour.rgb * (scene_brightness + spotLight * vec3(1.0, 0.95, 0.82));
        fragColour.rgb = mix(lit, textureColour.rgb, clamp(glowMask, 0.0, 1.0) * (1.0 - scene_brightness));
    }

    /* Perform gamma correction */
#if ENABLE_GAMMA_CORRECTION
    fragColour.rgb = pow(fragColour.rgb, vec3(1.0 / 1.2));
    // fragColour = adjustContrast(fragColour, 0.1);
    // fragColour = adjustExposure(fragColour, 2.0);
#endif

#if ENABLE_SIMULATE_8BIT_COLOUR
    fragColour = floor(fragColour.rgb * vec3(15.0)) / vec3(15.0);
#endif
#if ENABLE_SIMULATE_16BIT_COLOUR
    float r = floor(fragColour.r * 31.0) / 31.0;
    float g = floor(fragColour.g * 63.0) / 63.0;
    float b = floor(fragColour.b * 31.0) / 31.0;
    fragColour = vec3(r, g, b);
#endif

    // Apply alpha transparency
    fragColour.a = alpha * coverage;

    // Apply lighting
    if (lighting == 1u && prelit == 0u) {
        fragColour.rgb *= vec3(ka, ka, ka);
    }

    float fog = doDistanceFog();

    if (distance_fade > 0.0) {
        float d = length(position.xyz);
        fragColour.a *= 1.0 - smoothstep(distance_fade * yon_z, yon_z, d);
    }

    // Lights glow in their own (unlit) colour. Fog hides them; darkness only dims them a little, so lights
    // stand out in the distance at night
    glowColour = vec4(0.0, 0.0, 0.0, fragColour.a);
    if (glowMask > 0.0) {
        float hidden = fog * (fog_colour == vec3(0.0) ? 0.4 : 1.0);
        glowColour.rgb = textureColour.rgb * glowMask * (1.0 - hidden);
    }
}
