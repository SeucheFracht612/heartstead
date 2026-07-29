#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec2 fragment_uv;
layout(location = 2) flat in uint fragment_voxel_type;
layout(location = 3) flat in uint fragment_light;
layout(location = 4) flat in uint fragment_state_bits;
layout(location = 5) in vec3 fragment_world_position;

layout(set = 0, binding = 0) uniform sampler2DArray terrain_textures;

struct GpuVoxelMaterial {
    uvec4 texture_layers_and_flags;
    vec4 base_color;
    vec4 surface_parameters;
};

layout(std430, set = 0, binding = 1) readonly buffer VoxelMaterialTable {
    GpuVoxelMaterial materials[];
} voxel_material_table;

layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} chunk;

const uint MATERIAL_ALPHA_TESTED = 1U;
const uint MATERIAL_TRANSLUCENT = 2U;

vec3 linear_to_srgb(vec3 linear_color) {
    vec3 low = linear_color * 12.92;
    vec3 high = 1.055 * pow(max(linear_color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear_color, vec3(0.0031308)));
}

void main() {
    uint table_length = uint(voxel_material_table.materials.length());
    uint material_index = min(fragment_voxel_type, max(table_length, 1U) - 1U);
    GpuVoxelMaterial material = voxel_material_table.materials[material_index];

    vec3 normal = normalize(fragment_normal);
    uint texture_layer = material.texture_layers_and_flags.x;
    if (normal.y > 0.5) {
        texture_layer = material.texture_layers_and_flags.y;
    } else if (normal.y < -0.5) {
        texture_layer = material.texture_layers_and_flags.z;
    }
    vec4 texel = texture(terrain_textures, vec3(fragment_uv, float(texture_layer)));
    uint material_flags = material.texture_layers_and_flags.w;
    float surface_alpha = texel.a * material.base_color.a;
    if ((material_flags & MATERIAL_ALPHA_TESTED) != 0U && surface_alpha < 0.5) {
        discard;
    }

    vec3 light_direction = normalize(chunk.sun_direction_intensity.xyz);
    float directional = chunk.sun_direction_intensity.w * max(dot(normal, light_direction), 0.0);
    float baked_light = 0.55 + 0.45 * (float(fragment_light) / 255.0);
    float state_tint = (fragment_state_bits & 1U) != 0U ? 0.94 : 1.0;
    float emissive = material.surface_parameters.x;
    vec3 albedo = texel.rgb * material.base_color.rgb;
    vec3 lighting = chunk.ambient_color_fog_start.rgb + vec3(directional);
    vec3 lit = albedo * lighting * baked_light * state_tint;
    vec3 color = mix(lit, albedo, clamp(emissive, 0.0, 1.0));
    float fog_distance = length(fragment_world_position);
    float fog = smoothstep(chunk.ambient_color_fog_start.w, chunk.fog_color_fog_end.w,
                           fog_distance);
    color = mix(color, chunk.fog_color_fog_end.rgb, fog);
    float output_alpha = (material_flags & MATERIAL_TRANSLUCENT) != 0U ? surface_alpha : 1.0;
    out_color = vec4(linear_to_srgb(max(color, vec3(0.0))), output_alpha);
}
