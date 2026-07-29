#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec3 fragment_world_position;
layout(location = 2) in vec2 fragment_uv;
layout(location = 3) in vec4 fragment_color;
layout(location = 4) flat in uint fragment_layer;
layout(location = 5) flat in uint fragment_material;

layout(set = 0, binding = 2) uniform sampler2DArray surface_textures;

struct GpuSurfaceMaterial {
    uint base_color_texture;
    uint flags;
    float alpha_cutoff;
    float padding;
    vec4 base_color;
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

const uint LAYER_ALPHA_TESTED = 1U;
const uint LAYER_TRANSPARENT = 2U;
const uint MATERIAL_ALPHA_TESTED = 1U;
const uint MATERIAL_TWO_SIDED = 8U;

vec3 linear_to_srgb(vec3 linear_color) {
    vec3 low = linear_color * 12.92;
    vec3 high = 1.055 * pow(max(linear_color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear_color, vec3(0.0031308)));
}

void main() {
    GpuSurfaceMaterial material = surface_materials.materials[fragment_material];
    vec4 texel =
        texture(surface_textures, vec3(fragment_uv, float(material.base_color_texture)));
    vec4 base_color = texel * material.base_color * fragment_color;
    if ((material.flags & MATERIAL_ALPHA_TESTED) != 0U &&
        base_color.a < material.alpha_cutoff) {
        discard;
    }
    vec3 normal = normalize(fragment_normal);
    if ((material.flags & MATERIAL_TWO_SIDED) != 0U && !gl_FrontFacing) {
        normal = -normal;
    }
    vec3 sun_direction = normalize(frame.sun_direction_intensity.xyz);
    float directional = frame.sun_direction_intensity.w *
                        max(dot(normal, sun_direction), 0.0);
    vec3 color = base_color.rgb *
                 (frame.ambient_color_fog_start.rgb + vec3(directional));
    float fog = smoothstep(frame.ambient_color_fog_start.w, frame.fog_color_fog_end.w,
                           length(fragment_world_position));
    color = mix(color, frame.fog_color_fog_end.rgb, fog);
    float alpha = fragment_layer == LAYER_TRANSPARENT ? base_color.a : 1.0;
    out_color = vec4(linear_to_srgb(max(color, vec3(0.0))), alpha);
}
