#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec4 fragment_tangent;
layout(location = 2) in vec3 fragment_world_position;
layout(location = 3) in vec2 fragment_uv0;
layout(location = 4) in vec2 fragment_uv1;
layout(location = 5) in vec4 fragment_color;
layout(location = 6) flat in uint fragment_layer;
layout(location = 7) flat in uint fragment_material;

layout(set = 0, binding = 2) uniform sampler2DArray surface_textures;
layout(set = 0, binding = 4) uniform sampler2DArray surface_data_textures;

struct GpuTextureBinding {
    uvec4 metadata;
    vec4 transform;
};

struct GpuSurfaceMaterial {
    GpuTextureBinding textures[5];
    vec4 base_color;
    vec4 emissive_metallic;
    vec4 roughness_normal_occlusion_alpha;
    uint flags;
    uvec3 padding;
};

layout(std430, set = 0, binding = 3) readonly buffer SurfaceMaterials {
    GpuSurfaceMaterial materials[];
} surface_materials;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec4 out_color;

const uint LAYER_TRANSPARENT = 2U;
const uint MATERIAL_ALPHA_TESTED = 1U;
const uint MATERIAL_TWO_SIDED = 8U;
const uint MATERIAL_UNLIT = 32U;
const float PI = 3.14159265358979323846;

vec2 binding_uv(GpuTextureBinding binding) {
    vec2 uv = binding.metadata.z == 0U ? fragment_uv0 : fragment_uv1;
    uv *= binding.transform.zw;
    float rotation = uintBitsToFloat(binding.metadata.w);
    float sine = sin(rotation);
    float cosine = cos(rotation);
    return binding.transform.xy +
           mat2(cosine, sine, -sine, cosine) * uv;
}

int wrapped_coordinate(int coordinate, int extent, uint mode) {
    if (mode == 1U) {
        return clamp(coordinate, 0, extent - 1);
    }
    int repeated = coordinate % extent;
    if (repeated < 0) {
        repeated += extent;
    }
    if (mode == 0U) {
        return repeated;
    }
    int mirrored = coordinate % (extent * 2);
    if (mirrored < 0) {
        mirrored += extent * 2;
    }
    return mirrored < extent ? mirrored : extent * 2 - 1 - mirrored;
}

vec4 fetch_wrapped(sampler2DArray image, uint layer, ivec2 coordinate, int level,
                   uint sampler_state) {
    ivec2 extent = textureSize(image, level).xy;
    uint wrap_s = (sampler_state >> 4U) & 3U;
    uint wrap_t = (sampler_state >> 6U) & 3U;
    ivec2 wrapped = ivec2(wrapped_coordinate(coordinate.x, extent.x, wrap_s),
                          wrapped_coordinate(coordinate.y, extent.y, wrap_t));
    return texelFetch(image, ivec3(wrapped, int(layer)), level);
}

vec4 sample_level(sampler2DArray image, uint layer, vec2 uv, int level,
                  uint sampler_state, bool linear_filter) {
    ivec2 extent = textureSize(image, level).xy;
    vec2 position = uv * vec2(extent) - vec2(0.5);
    ivec2 first = ivec2(floor(position));
    if (!linear_filter) {
        return fetch_wrapped(image, layer, ivec2(floor(position + vec2(0.5))), level,
                             sampler_state);
    }
    vec2 fraction = fract(position);
    vec4 top = mix(fetch_wrapped(image, layer, first, level, sampler_state),
                   fetch_wrapped(image, layer, first + ivec2(1, 0), level, sampler_state),
                   fraction.x);
    vec4 bottom =
        mix(fetch_wrapped(image, layer, first + ivec2(0, 1), level, sampler_state),
            fetch_wrapped(image, layer, first + ivec2(1, 1), level, sampler_state),
            fraction.x);
    return mix(top, bottom, fraction.y);
}

vec4 sample_binding(sampler2DArray image, GpuTextureBinding binding) {
    vec2 uv = binding_uv(binding);
    uint sampler_state = binding.metadata.y;
    uint layer = binding.metadata.x;
    // Array layers do not participate in implicit derivatives, so the query
    // takes the two-dimensional texture coordinate.
    float lod = max(textureQueryLod(image, uv).x, 0.0);
    int maximum_level = textureQueryLevels(image) - 1;
    if (lod <= 0.0) {
        bool linear_filter = (sampler_state & 1U) != 0U;
        return sample_level(image, layer, uv, 0, sampler_state, linear_filter);
    }
    uint min_filter = (sampler_state >> 1U) & 7U;
    if (min_filter <= 1U) {
        return sample_level(image, layer, uv, 0, sampler_state, min_filter == 1U);
    }
    bool linear_filter = min_filter == 3U || min_filter == 5U;
    float clamped_lod = clamp(lod, 0.0, float(maximum_level));
    if (min_filter == 2U || min_filter == 3U) {
        int level = int(floor(clamped_lod + 0.5));
        return sample_level(image, layer, uv, level, sampler_state, linear_filter);
    }
    int first_level = int(floor(clamped_lod));
    int second_level = min(first_level + 1, maximum_level);
    vec4 first = sample_level(image, layer, uv, first_level, sampler_state,
                              linear_filter);
    vec4 second = sample_level(image, layer, uv, second_level, sampler_state,
                               linear_filter);
    return mix(first, second, fract(clamped_lod));
}

vec3 linear_to_srgb(vec3 linear_color) {
    vec3 low = linear_color * 12.92;
    vec3 high = 1.055 * pow(max(linear_color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear_color, vec3(0.0031308)));
}

void main() {
    GpuSurfaceMaterial material = surface_materials.materials[fragment_material];
    vec4 base_color =
        sample_binding(surface_textures, material.textures[0]) *
        material.base_color * fragment_color;
    if ((material.flags & MATERIAL_ALPHA_TESTED) != 0U &&
        base_color.a < material.roughness_normal_occlusion_alpha.w) {
        discard;
    }
    vec3 emissive =
        sample_binding(surface_textures, material.textures[4]).rgb *
        material.emissive_metallic.rgb;
    vec3 color = base_color.rgb + emissive;
    if ((material.flags & MATERIAL_UNLIT) == 0U) {
        vec3 normal = normalize(fragment_normal);
        if ((material.flags & MATERIAL_TWO_SIDED) != 0U && !gl_FrontFacing) {
            normal = -normal;
        }
        vec3 tangent = normalize(fragment_tangent.xyz -
                                 normal * dot(normal, fragment_tangent.xyz));
        vec3 bitangent = normalize(cross(normal, tangent)) * fragment_tangent.w;
        vec3 tangent_normal =
            sample_binding(surface_data_textures, material.textures[2]).xyz *
                2.0 -
            1.0;
        tangent_normal.xy *= material.roughness_normal_occlusion_alpha.y;
        normal = normalize(mat3(tangent, bitangent, normal) * tangent_normal);

        vec4 metallic_roughness =
            sample_binding(surface_data_textures, material.textures[1]);
        float metallic =
            clamp(material.emissive_metallic.w * metallic_roughness.b, 0.0, 1.0);
        float roughness =
            clamp(material.roughness_normal_occlusion_alpha.x *
                      metallic_roughness.g,
                  0.045, 1.0);
        float occlusion = mix(
            1.0,
            sample_binding(surface_data_textures, material.textures[3]).r,
            material.roughness_normal_occlusion_alpha.z);

        vec3 view_direction = normalize(-fragment_world_position);
        vec3 light_direction = normalize(frame.sun_direction_intensity.xyz);
        vec3 halfway = normalize(view_direction + light_direction);
        float normal_light = max(dot(normal, light_direction), 0.0);
        float normal_view = max(dot(normal, view_direction), 0.0001);
        float normal_half = max(dot(normal, halfway), 0.0);
        float view_half = max(dot(view_direction, halfway), 0.0);
        float alpha = roughness * roughness;
        float alpha_squared = alpha * alpha;
        float denominator =
            normal_half * normal_half * (alpha_squared - 1.0) + 1.0;
        float distribution =
            alpha_squared / max(PI * denominator * denominator, 0.0001);
        float geometry_k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
        float geometry_view =
            normal_view / (normal_view * (1.0 - geometry_k) + geometry_k);
        float geometry_light =
            normal_light / (normal_light * (1.0 - geometry_k) + geometry_k);
        vec3 fresnel_base = mix(vec3(0.04), base_color.rgb, metallic);
        vec3 fresnel =
            fresnel_base + (1.0 - fresnel_base) * pow(1.0 - view_half, 5.0);
        vec3 specular = distribution * geometry_view * geometry_light * fresnel /
                        max(4.0 * normal_view * normal_light, 0.0001);
        vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) *
                       base_color.rgb / PI;
        vec3 direct = (diffuse + specular) * normal_light *
                      frame.sun_direction_intensity.w;
        vec3 ambient =
            frame.ambient_color_fog_start.rgb *
            (base_color.rgb * (1.0 - metallic) + fresnel_base * 0.25) *
            occlusion;
        color = ambient + direct + emissive;
    }
    float fog = smoothstep(frame.ambient_color_fog_start.w,
                           frame.fog_color_fog_end.w,
                           length(fragment_world_position));
    color = mix(color, frame.fog_color_fog_end.rgb, fog);
    float alpha = fragment_layer == LAYER_TRANSPARENT ? base_color.a : 1.0;
    out_color = vec4(linear_to_srgb(max(color, vec3(0.0))), alpha);
}
