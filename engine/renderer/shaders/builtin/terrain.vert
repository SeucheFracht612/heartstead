#version 450

// Compact GpuTerrainVertex contract (engine/renderer/terrain/gpu_chunk_vertex.hpp).
layout(location = 0) in ivec4 in_position_fixed;
layout(location = 1) in uvec2 in_uv_fixed;
layout(location = 2) in uint in_material;
layout(location = 3) in uint in_state_bits;
layout(location = 4) in vec4 in_normal_snorm;
layout(location = 5) in uvec4 in_lighting;

layout(location = 0) out vec3 fragment_normal;
layout(location = 1) out vec2 fragment_uv;
layout(location = 2) flat out uint fragment_voxel_type;
layout(location = 3) flat out uint fragment_light;
layout(location = 4) flat out uint fragment_state_bits;
layout(location = 5) out vec3 fragment_world_position;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} chunk;

void main() {
    vec3 in_position = vec3(in_position_fixed.xyz) / 256.0;
    vec2 in_uv = vec2(in_uv_fixed) / 256.0;
    vec3 world_position = in_position + chunk.camera_relative_origin.xyz;
    gl_Position = chunk.view_projection * vec4(world_position, 1.0);
    fragment_normal = normalize(in_normal_snorm.xyz);
    fragment_uv = in_uv;
    fragment_voxel_type = in_material;
    fragment_light = in_lighting.x;
    fragment_state_bits = in_state_bits;
    fragment_world_position = world_position;
}
