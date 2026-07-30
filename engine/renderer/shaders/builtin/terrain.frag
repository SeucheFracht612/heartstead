#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec2 fragment_uv;
layout(location = 2) flat in uint fragment_voxel_type;
layout(location = 3) flat in uint fragment_light;
layout(location = 4) flat in uint fragment_state_bits;
layout(location = 5) in vec3 fragment_world_position;

layout(set = 0, binding = 0) uniform sampler2DArray terrain_textures;

struct GpuVoxelMaterial {
    uvec4 face_texture_starts_0;
    uvec4 face_texture_starts_1;
    uvec4 face_texture_counts_0;
    uvec4 face_texture_counts_1;
    uvec4 flags_and_padding;
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

// Writes linear radiance into the HDR scene target. Display encoding happens once, in the
// tone mapping pass; a transfer function applied here would be applied twice.
uint face_index(vec3 normal) {
    vec3 magnitude = abs(normal);
    if (magnitude.y >= magnitude.x && magnitude.y >= magnitude.z) {
        return normal.y < 0.0 ? 2U : 3U;
    }
    if (magnitude.x >= magnitude.z) {
        return normal.x < 0.0 ? 0U : 1U;
    }
    return normal.z < 0.0 ? 4U : 5U;
}

uint face_table_value(uvec4 first, uvec4 second, uint face) {
    return face < 4U ? first[face] : second[face - 4U];
}

uint texture_variant_hash(ivec3 local_cell, uint face, uint chunk_seed) {
    uint hash = chunk_seed ^ (face * 0x9e3779b9U);
    hash ^= uint(local_cell.x) * 0x85ebca6bU;
    hash ^= uint(local_cell.y) * 0xc2b2ae35U;
    hash ^= uint(local_cell.z) * 0x27d4eb2fU;
    hash ^= hash >> 16U;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15U;
    hash *= 0x846ca68bU;
    return hash ^ (hash >> 16U);
}

void main() {
    uint table_length = uint(voxel_material_table.materials.length());
    uint material_index = min(fragment_voxel_type, max(table_length, 1U) - 1U);
    GpuVoxelMaterial material = voxel_material_table.materials[material_index];

    vec3 normal = normalize(fragment_normal);
    uint face = face_index(normal);
    uint texture_start =
        face_table_value(material.face_texture_starts_0, material.face_texture_starts_1, face);
    uint texture_count =
        max(face_table_value(material.face_texture_counts_0,
                             material.face_texture_counts_1, face),
            1U);
    vec3 local_position = fragment_world_position - chunk.camera_relative_origin.xyz;
    ivec3 local_cell = ivec3(floor(local_position - normal * 0.001));
    uint variant =
        texture_variant_hash(local_cell, face, floatBitsToUint(chunk.camera_relative_origin.w)) %
        texture_count;
    uint texture_layer = texture_start + variant;
    vec4 texel = texture(terrain_textures, vec3(fragment_uv, float(texture_layer)));
    uint material_flags = material.flags_and_padding.x;
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
    out_color = vec4(max(color, vec3(0.0)), output_alpha);
}
