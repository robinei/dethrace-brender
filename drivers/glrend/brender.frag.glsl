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
// How much this surface mirrors its surroundings (ice), written to the glow attachment's alpha as 1 - it for
// the post-processing; below 0 when blending (the alpha then blends the glow, and the mask is left alone), and
// -0.25 for car paint that doesn't mirror
uniform float surface_reflect;
// Soft particles: the depth of the solid scene (drawn before the smoke), and the distance in front of it over
// which a blended draw fades out; 0 for off
uniform sampler2D soft_depth;
uniform float soft_distance;
// Car wear (0 for none, up to 2): dirt and scratches on the paint as the car is damaged
uniform float surface_wear;
// Drawn as a sprite (a flat card facing the camera: the pedestrians), marked for the sun shadows
uniform bool surface_sprite;
// Night: the scene is darkened to scene_brightness, except for light source texels, and lit by lights (car
// headlights, fires) in view space: position + range, direction (zero for a point light) + strength,
// colour + haze
#define MAX_SPOTS 32
uniform float scene_brightness;
uniform int num_spots;
uniform vec4 spots[3 * MAX_SPOTS];
// how much the lights light this fragment; also lifts the darkness depth cue
vec3 spotLight = vec3(0.0);
// flashes (lights with a negative haze: sparks, fires flaring up): they light beyond the daylight brightness,
// so they show by day too
vec3 flashLight = vec3(0.0);

float spotCone(vec3 from_light, vec4 dir) {
    return dot(dir.xyz, dir.xyz) < 0.25 ? 1.0 : smoothstep(0.82, 0.95, dot(from_light, dir.xyz));
}

vec3 spotLighting(vec3 p, vec3 n) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < num_spots; i++) {
        vec3 to_light = spots[3 * i].xyz - p;
        float d = length(to_light);
        float range = spots[3 * i].w;
        if (d >= range) {
            continue;
        }
        vec3 l = to_light / max(d, 0.0001);
        float falloff = 1.0 - d / range;
        float facing = dot(n, n) > 0.01 ? max(dot(n, l), 0.0) * 0.8 + 0.2 : 1.0;
        vec3 c = spots[3 * i + 2].rgb * (spotCone(-l, spots[3 * i + 1]) * falloff * falloff * facing * spots[3 * i + 1].w);
        if (spots[3 * i + 2].w < 0.0) {
            flashLight += c;
        } else {
            sum += c;
        }
    }
    return sum;
}

// Blob shadows (entries with a negative range: the radius; direction: the world's up; strength): darken the
// surfaces facing up within the radius around the point, from a little above it to a radius below (the ground
// under a pedestrian). Not the sprites themselves (no normal)
float blobShadow(vec3 p, vec3 n) {
    float shade = 1.0;
    if (!(dot(n, n) > 0.01)) {
        return 1.0;
    }
    for (int i = 0; i < num_spots; i++) {
        float radius = -spots[3 * i].w;
        if (radius <= 0.0) {
            continue;
        }
        vec3 up = spots[3 * i + 1].xyz;
        vec3 v = p - spots[3 * i].xyz;
        float h = dot(v, up);
        float r = length(v - up * h) / radius;
        if (r >= 1.0 || h >= 0.3 * radius || h <= -1.2 * radius) {
            continue;
        }
        float facing = smoothstep(0.4, 0.8, abs(dot(n, up)));
        float disc = 1.0 - smoothstep(0.3, 1.0, r);
        float near = h > 0.0 ? 1.0 - smoothstep(0.1 * radius, 0.3 * radius, h) : 1.0 - smoothstep(0.6 * radius, 1.2 * radius, -h);
        shade *= 1.0 - spots[3 * i + 1].w * disc * near * facing;
    }
    return shade;
}

// The textured surfaces with a normal (the track, scenery and cars, not sprites): darker facing away from the
// sun (direction xyz, amount w), and a large scale brightness variation over their model space (amount
// surface_up.w) to break up tiled textures
uniform vec4 surface_sun;
uniform vec4 surface_up;  // the world's up in view space
uniform vec4 surface_haze[2]; // haze colour, most of it; the distance most of it is reached by, relief strength

float hash3(vec3 p) {
    p = fract(p * 0.3183099 + vec3(0.71, 0.113, 0.419));
    p *= 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}

float valueNoise(vec3 x) {
    vec3 i = floor(x);
    vec3 f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i), hash3(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(hash3(i + vec3(0.0, 1.0, 0.0)), hash3(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0.0, 0.0, 1.0)), hash3(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(hash3(i + vec3(0.0, 1.0, 1.0)), hash3(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}

// Relief: the texture's brightness taken as height (bright is raised), how much brighter or darker the sun makes
// the surface by it, by the normal tilted along its gradient (without tangents: from the screen derivatives).
// Fades out where the texture is minified, where it would only shimmer. Read around `at` (uv, or snapped to
// the texels' centres with the sharp filter)
float relief(vec3 p, vec3 n, vec2 uv, vec2 at) {
    vec2 size = vec2(textureSize(main_texture, 0));
    // (the derivatives of the unsnapped coordinates, for the mip level and the screen to texture mapping)
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    float texels = max(length(dx * size), length(dy * size));
    float fade = 1.0 - smoothstep(0.8, 2.5, texels);
    if (fade <= 0.0) {
        return 0.0;
    }
    vec2 t = 1.0 / size;
    const vec3 lum = vec3(0.299, 0.587, 0.114);
    float hl = dot(textureGrad(main_texture, at - vec2(t.x, 0.0), dx, dy).rgb, lum);
    float hr = dot(textureGrad(main_texture, at + vec2(t.x, 0.0), dx, dy).rgb, lum);
    float hd = dot(textureGrad(main_texture, at - vec2(0.0, t.y), dx, dy).rgb, lum);
    float hu = dot(textureGrad(main_texture, at + vec2(0.0, t.y), dx, dy).rgb, lum);
    // the height's change per uv, then per window pixel; in world units (a height range of about 35cm: exaggerated)
    vec2 dh = vec2(hr - hl, hu - hd) / (2.0 * t) * 0.05 * surface_haze[1].y;
    float dhdx = dot(dh, dx);
    float dhdy = dot(dh, dy);
    vec3 dpdx = dFdx(p);
    vec3 dpdy = dFdy(p);
    vec3 r1 = cross(dpdy, n);
    vec3 r2 = cross(n, dpdx);
    float det = dot(dpdx, r1);
    if (abs(det) < 1e-20) {
        return 0.0;
    }
    vec3 nb = normalize(abs(det) * n - sign(det) * (dhdx * r1 + dhdy * r2));
    return (dot(nb, surface_sun.xyz) - dot(n, surface_sun.xyz)) * fade;
}

// Car wear: patches of grime (darker, browner, duller) spreading with the wear, and fine scratches catching the
// light, in the car's model space so they stay put on its body
vec3 carWear(vec3 c, vec3 model_pos) {
    float w = surface_wear;
    float lo = 0.72 - 0.32 * min(w, 1.5);
    float grime = smoothstep(lo, lo + 0.18, valueNoise(model_pos * 9.0) * 0.7 + valueNoise(model_pos * 31.0) * 0.3);
    vec3 dirty = mix(c, vec3(dot(c, vec3(0.3, 0.59, 0.11))) * vec3(0.62, 0.52, 0.4), 0.75);
    c = mix(c, dirty, grime * min(w, 1.0));
    // scratches: thin streaks along a slanting direction, in short pieces
    vec3 q = model_pos * 40.0;
    float line = abs(fract(q.x * 0.8 + q.y * 0.35 + q.z * 0.6 + valueNoise(q * 0.08) * 3.0) - 0.5);
    float piece = smoothstep(1.0 - 0.3 * min(w, 2.0), 1.05 - 0.3 * min(w, 2.0), valueNoise(q * 0.12 + 3.0));
    float scratch = (1.0 - smoothstep(0.0, 0.07, line)) * piece;
    return mix(c, vec3(0.62, 0.6, 0.57), scratch * 0.55 * min(w, 1.0));
}

vec3 surfaceShading(vec3 model_pos, vec3 p, vec3 vertex_n, vec2 uv, vec2 relief_uv) {
    vec3 shade = vec3(1.0);
    if (surface_sun.w > 0.0) {
        // relative to the ground, which keeps its brightness: walls turned away from the sun darken. The face's
        // own (flat) normal, of the side seen; and the (smooth) vertex normal turned to the same side, where it
        // is close to it: across gentle folds (faceted rock, banks) the shading is smooth, at creases (the road
        // against a wall, where the vertex normals are averaged to halfway) it stays the face's own
        vec3 n = cross(dFdx(p), dFdy(p));
        if (dot(n, n) < 1e-20) {
            return shade;
        }
        n = normalize(n);
        if (dot(n, -p) < 0.0) {
            n = -n;
        }
        if (dot(vertex_n, vertex_n) > 0.01) {
            vec3 v = normalize(vertex_n);
            float c = dot(v, n);
            if (c < 0.0) {
                v = -v;
                c = -c;
            }
            n = normalize(mix(n, v, smoothstep(0.8, 0.95, c)));
        }
        float k = dot(n, surface_sun.xyz) - dot(surface_up.xyz, surface_sun.xyz);
        shade *= clamp(1.0 + surface_sun.w * 0.2 * k, 0.6, 1.04);
        // (not on the cars: their paint is smooth, and their textures' mottling would only make it look scaly)
        if (surface_haze[1].y > 0.0 && abs(abs(surface_reflect) - 0.25) > 0.01) {
            shade *= clamp(1.0 + 2.0 * relief(p, n, uv, relief_uv), 0.6, 1.4);
        }
    }
    if (surface_up.w > 0.0) {
        // patches several metres across (the world unit is about 7m), two octaves, and a hint of warm/cool
        // (the large patches stronger than the finer ones)
        float v = 0.14 * (valueNoise(model_pos * 0.6) - 0.5) + 0.05 * (valueNoise(model_pos * 1.9 + 17.0) - 0.5);
        float t = valueNoise(model_pos * 0.35 + 5.0);
        shade *= 1.0 + surface_up.w * v;
        shade *= vec3(1.0 + surface_up.w * 0.012 * (t - 0.5), 1.0, 1.0 - surface_up.w * 0.012 * (t - 0.5));
    }
    return shade;
}

// Light scattered towards the eye by the air between it and p: for each light, the integral of 1/distance^2
// along the view ray, weighted by the cone at the ray's closest point to the light
vec3 spotHaze(vec3 p) {
    vec3 sum = vec3(0.0);
    float len = length(p);
    vec3 r = p / max(len, 0.0001);
    for (int i = 0; i < num_spots; i++) {
        float haze = spots[3 * i + 2].w;
        if (haze <= 0.0) {
            continue;
        }
        vec3 c = spots[3 * i].xyz;
        float range = spots[3 * i].w;
        float b = dot(c, r);
        float h = sqrt(max(dot(c, c) - b * b, 0.0004));
        float scatter = (atan((len - b) / h) + atan(b / h)) / h;
        vec3 q = r * clamp(b, 0.0, len);
        vec3 from_light = q - c;
        float d = length(from_light);
        float falloff = clamp(1.0 - d / range, 0.0, 1.0);
        sum += spots[3 * i + 2].rgb * (haze * scatter * falloff * spotCone(from_light / max(d, 0.0001), spots[3 * i + 1]));
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
            vec3 l = spotLight + flashLight;
            f *= 1.0 - clamp(max(l.r, max(l.g, l.b)), 0.0, 1.0);
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
    // where the relief reads the texture: with the sharp filter, snapped like the colour, so it is flat within
    // each texel too (each a little tile tilted its own way) and the texels stay square
    vec2 reliefUV = mappedUV;

    if (disable_texture) {
        // Lit, non-prelit vertices already include the material colour.
        textureColour = (lighting == 1u && prelit == 0u) ? vec4(1.0) : surface_colour;
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
            reliefUV = snapped;
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
    // (the track materials are lit or not as their files say, so this goes by whether there is a normal:
    // not for the sprites)
    if ((surface_sun.w > 0.0 || surface_up.w > 0.0) && !disable_texture && dot(normal.xyz, normal.xyz) > 0.01) {
        fragColour.rgb *= surfaceShading(rawPosition, position.xyz, normal.xyz, mappedUV, reliefUV);
    }

    if (surface_wear > 0.0 && !disable_texture) {
        fragColour.rgb = carWear(fragColour.rgb, rawPosition);
    }

    vec3 flashColour = vec3(0.0);
    float glowMask = 0.0;
    if (glow_intensity > 0.0 && !disable_texture) {
        glowMask = texture(glow_texture, mappedUV).r * glow_intensity;
    }
    if (scene_brightness < 1.0 || num_spots > 0) {
        // night: dark, lit by the headlights (warm white), light sources keep their own brightness
        if (num_spots > 0) {
            spotLight = spotLighting(position.xyz, normalize(normal.xyz));
        }
        // lit up to the normal (daylight) brightness, not beyond, so close bright surfaces don't blow out
        vec3 lit = fragColour.rgb * min(scene_brightness + spotLight, vec3(1.0));
        flashColour = fragColour.rgb * flashLight;
        lit += flashColour;
        fragColour.rgb = mix(lit, textureColour.rgb, clamp(glowMask, 0.0, 1.0) * (1.0 - scene_brightness));
        if (num_spots > 0) {
            fragColour.rgb *= blobShadow(position.xyz, normalize(normal.xyz));
        }
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

    if (surface_haze[0].w > 0.0) {
        // aerial perspective: everything takes on a little of the haze (sky) colour with distance, sharp
        float d = length(position.xyz);
        fragColour.rgb = mix(fragColour.rgb, surface_haze[0].rgb, surface_haze[0].w * (1.0 - exp(-2.0 * d / max(surface_haze[1].x, 1.0))));
    }

    float fog = doDistanceFog();
    if (num_spots > 0) {
        fragColour.rgb += spotHaze(position.xyz);
    }

    if (distance_fade > 0.0) {
        float d = length(position.xyz);
        fragColour.a *= 1.0 - smoothstep(distance_fade * yon_z, yon_z, d);
    }
    if (soft_distance > 0.0) {
        // the solid scene's distance here, from its depth by the projection (as the depth was made)
        float ndc = 2.0 * texelFetch(soft_depth, ivec2(gl_FragCoord.xy), 0).r - 1.0;
        float scene = projection[3][2] / (ndc + projection[2][2]);
        fragColour.a *= smoothstep(0.0, 1.0, clamp((scene + position.z) / soft_distance, 0.0, 1.0));
    }

    // Lights glow in their own (unlit) colour. Fog hides them; darkness only dims them a little, so lights
    // stand out in the distance at night
    glowColour = vec4(0.0, 0.0, 0.0, fragColour.a);
    if (glowMask > 0.0) {
        float hidden = fog * (fog_colour == vec3(0.0) ? 0.4 : 1.0);
        glowColour.rgb = textureColour.rgb * glowMask * (1.0 - hidden);
    }
    // what a flash lights glows a little too
    glowColour.rgb += flashColour * 0.25 * (1.0 - fog);
    if (surface_reflect >= 0.0) {
        // (the sprites are marked 1 - 0.04, for the sun shadows)
        glowColour.a = surface_sprite ? 0.96 : 1.0 - surface_reflect * (dot(normal.xyz, normal.xyz) > 0.01 ? 1.0 : 0.0);
    }
}
