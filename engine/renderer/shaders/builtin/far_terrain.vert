#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uint in_material;
layout(location = 4) in vec2 in_lod_blend;

layout(location = 0) out vec3 fragment_normal;
layout(location = 1) out vec2 fragment_uv;
layout(location = 2) flat out uint fragment_voxel_type;
layout(location = 3) flat out uint fragment_light;
layout(location = 4) flat out uint fragment_state_bits;
layout(location = 5) out vec3 fragment_world_position;
layout(location = 6) out float fragment_voxel_ao;
layout(location = 7) out float fragment_lod_blend;
layout(location = 8) flat out uint fragment_coordinate_key;

layout(std430, set = 0, binding = 15) readonly buffer FarPatchDraws {
    vec4 origin_and_key[];
} far_patch_draws;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} chunk;

void main() {
    const uint indirect_marker = 0x7fc00001U;
    bool indirect_draw = floatBitsToUint(chunk.camera_relative_origin.w) == indirect_marker;
    vec4 patch_data = indirect_draw
                          ? far_patch_draws.origin_and_key[gl_InstanceIndex]
                          : chunk.camera_relative_origin;
    vec3 world_position = in_position + patch_data.xyz;
    gl_Position = chunk.view_projection * vec4(world_position, 1.0);
    fragment_normal = normalize(in_normal);
    fragment_uv = in_uv;
    fragment_voxel_type = in_material;
    fragment_light = 255u;
    fragment_state_bits = 0u;
    fragment_world_position = world_position;
    fragment_voxel_ao = 1.0;
    fragment_lod_blend = clamp(in_lod_blend.x, 0.0, 1.0);
    fragment_coordinate_key = floatBitsToUint(patch_data.w);
}
