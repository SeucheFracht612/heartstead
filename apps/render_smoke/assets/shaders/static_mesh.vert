#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in uvec4 in_joints;
layout(location = 4) in vec4 in_weights;

struct GpuObjectInstance {
    mat4 camera_relative_transform;
    vec4 color;
    uvec4 metadata;
};

layout(std430, set = 0, binding = 0) readonly buffer ObjectInstances {
    GpuObjectInstance instances[];
} object_instances;

layout(std430, set = 0, binding = 1) readonly buffer SkinMatrices {
    mat4 matrices[];
} skin_matrices;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec3 fragment_normal;
layout(location = 1) out vec3 fragment_world_position;
layout(location = 2) out vec2 fragment_uv;
layout(location = 3) out vec4 fragment_color;
layout(location = 4) flat out uint fragment_layer;

void main() {
    GpuObjectInstance instance = object_instances.instances[gl_InstanceIndex];
    vec4 local_position = vec4(in_position, 1.0);
    vec3 local_normal = in_normal;
    uint skin_matrix_count = instance.metadata.z;
    if (skin_matrix_count > 0U) {
        uvec4 joints = min(in_joints, uvec4(skin_matrix_count - 1U));
        uint base = instance.metadata.y;
        mat4 skin = in_weights.x * skin_matrices.matrices[base + joints.x] +
                    in_weights.y * skin_matrices.matrices[base + joints.y] +
                    in_weights.z * skin_matrices.matrices[base + joints.z] +
                    in_weights.w * skin_matrices.matrices[base + joints.w];
        local_position = skin * local_position;
        local_normal = mat3(skin) * local_normal;
    }
    vec4 world_position = instance.camera_relative_transform * local_position;
    gl_Position = frame.view_projection * world_position;
    fragment_normal = normalize(transpose(inverse(mat3(instance.camera_relative_transform))) *
                                local_normal);
    fragment_world_position = world_position.xyz;
    fragment_uv = in_uv;
    fragment_color = instance.color;
    fragment_layer = instance.metadata.x;
}
