#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_normal;
layout(location = 2) in vec4 in_tangent;
layout(location = 3) in vec2 in_uv0;
layout(location = 4) in vec2 in_uv1;
layout(location = 5) in uvec4 in_joints;
layout(location = 6) in vec4 in_weights;
layout(location = 7) in vec4 in_color;

struct GpuObjectInstance {
    mat4 camera_relative_transform;
    vec4 color;
    uvec4 metadata;
    uvec4 morph_metadata;
    vec4 effect_parameters;
    uvec4 effect_metadata;
    vec4 effect_parameters2;
};

struct GpuMorphDelta {
    vec4 position;
    vec4 normal;
    vec4 tangent;
};

layout(std430, set = 0, binding = 0) readonly buffer ObjectInstances {
    GpuObjectInstance instances[];
} object_instances;

layout(std430, set = 0, binding = 1) readonly buffer SkinMatrices {
    mat4 matrices[];
} skin_matrices;

layout(std430, set = 0, binding = 5) readonly buffer MorphDeltas {
    GpuMorphDelta deltas[];
} morph_deltas;

layout(std430, set = 0, binding = 6) readonly buffer MorphWeights {
    float weights[];
} morph_weights;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec3 fragment_normal;
layout(location = 1) out vec4 fragment_tangent;
layout(location = 2) out vec3 fragment_world_position;
layout(location = 3) out vec2 fragment_uv0;
layout(location = 4) out vec2 fragment_uv1;
layout(location = 5) out vec4 fragment_color;
layout(location = 6) flat out uint fragment_layer;
layout(location = 7) flat out uint fragment_material;
layout(location = 8) out vec4 fragment_skin_weights;
layout(location = 9) out vec4 fragment_effect_parameters;
layout(location = 10) flat out uvec4 fragment_effect_metadata;
layout(location = 11) out vec4 fragment_effect_parameters2;

const uint EFFECT_VEGETATION = 1U;
const uint EFFECT_WATER_SURFACE = 64U;

void main() {
    GpuObjectInstance instance = object_instances.instances[gl_InstanceIndex];
    vec3 local_position = in_position;
    vec3 local_normal = in_normal.xyz;
    vec3 local_tangent = in_tangent.xyz;
    if ((instance.effect_metadata.x & EFFECT_VEGETATION) != 0U) {
        vec2 wind = frame.unused_origin.xy;
        float wind_speed = length(wind);
        if (wind_speed > 0.0001) {
            vec2 wind_direction = wind / wind_speed;
            vec2 anchor = instance.camera_relative_transform[3].xz;
            float height_weight =
                1.0 - exp(-max(local_position.y, 0.0) * 0.85);
            float spatial_phase =
                dot(anchor, vec2(0.071, 0.053)) +
                instance.effect_parameters.x;
            float primary =
                sin(frame.unused_origin.z * (0.9 + wind_speed * 0.08) +
                    spatial_phase);
            float detail =
                sin(frame.unused_origin.z * 2.3 +
                    spatial_phase * 2.17) *
                0.28;
            float flexibility =
                1.0 - clamp(instance.effect_parameters.y, 0.0, 1.0);
            local_position.xz +=
                wind_direction * (primary + detail) * wind_speed *
                flexibility * height_weight * 0.075;
        }
    }
    if ((instance.effect_metadata.x & EFFECT_WATER_SURFACE) != 0U) {
        vec3 approximate_world =
            (instance.camera_relative_transform * vec4(local_position, 1.0)).xyz;
        float time = frame.unused_origin.z * instance.effect_parameters2.y;
        float first = sin(dot(approximate_world.xz, vec2(0.071, 0.043)) +
                          time * 1.17);
        float second = sin(dot(approximate_world.xz, vec2(-0.037, 0.091)) -
                           time * 0.83);
        float detail = sin(dot(approximate_world.xz, vec2(0.19, -0.14)) +
                           time * 1.91);
        local_position.y +=
            (first + second * 0.62 + detail * 0.18) *
            instance.effect_parameters2.x;
    }
    uint vertex_count = instance.morph_metadata.w & 0x00ffffffU;
    uint morph_count = instance.morph_metadata.w >> 24U;
    if (morph_count > 0U) {
        uint local_vertex = uint(gl_VertexIndex) - instance.morph_metadata.x;
        for (uint target = 0U; target < morph_count; ++target) {
            float weight = morph_weights.weights[instance.morph_metadata.z + target];
            GpuMorphDelta delta =
                morph_deltas.deltas[instance.morph_metadata.y + target * vertex_count +
                                    local_vertex];
            local_position += delta.position.xyz * weight;
            local_normal += delta.normal.xyz * weight;
            local_tangent += delta.tangent.xyz * weight;
        }
    }

    vec4 skinned_position = vec4(local_position, 1.0);
    uint skin_matrix_count = instance.metadata.z;
    if (skin_matrix_count > 0U) {
        uvec4 joints = min(in_joints, uvec4(skin_matrix_count - 1U));
        uint base = instance.metadata.y;
        mat4 skin = in_weights.x * skin_matrices.matrices[base + joints.x] +
                    in_weights.y * skin_matrices.matrices[base + joints.y] +
                    in_weights.z * skin_matrices.matrices[base + joints.z] +
                    in_weights.w * skin_matrices.matrices[base + joints.w];
        skinned_position = skin * skinned_position;
        local_normal = mat3(skin) * local_normal;
        local_tangent = mat3(skin) * local_tangent;
    }
    vec4 world_position = instance.camera_relative_transform * skinned_position;
    mat3 normal_matrix = transpose(inverse(mat3(instance.camera_relative_transform)));
    gl_Position = frame.view_projection * world_position;
    fragment_normal = normalize(normal_matrix * local_normal);
    fragment_tangent =
        vec4(normalize(normal_matrix * local_tangent), in_tangent.w);
    fragment_world_position = world_position.xyz;
    uvec2 atlas = max(instance.effect_metadata.zw, uvec2(1U));
    uint frame_count = atlas.x * atlas.y;
    uint sprite_frame =
        min(instance.effect_metadata.y, max(frame_count, 1U) - 1U);
    vec2 atlas_cell =
        vec2(sprite_frame % atlas.x, sprite_frame / atlas.x);
    fragment_uv0 = (in_uv0 + atlas_cell) / vec2(atlas);
    fragment_uv1 = (in_uv1 + atlas_cell) / vec2(atlas);
    fragment_color = instance.color * in_color;
    fragment_layer = instance.metadata.x;
    fragment_material = instance.metadata.w;
    fragment_skin_weights = in_weights;
    fragment_effect_parameters = instance.effect_parameters;
    fragment_effect_metadata = instance.effect_metadata;
    fragment_effect_parameters2 = instance.effect_parameters2;
}
