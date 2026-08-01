#version 450

layout(location = 1) in vec2 fragment_uv;
layout(location = 2) flat in uint fragment_voxel_type;
layout(location = 3) flat in uint fragment_light;
layout(location = 4) flat in uint fragment_state_bits;
layout(location = 0) in vec3 fragment_normal;
layout(location = 5) in vec3 fragment_world_position;
layout(location = 6) in float fragment_voxel_ao;
layout(location = 7) in float fragment_lod_blend;
layout(location = 8) flat in uint fragment_coordinate_key;

layout(set = 0, binding = 0) uniform sampler2DArray terrain_textures;

struct GpuTerrainSurfaceLayer {
    uvec4 packed;
};

struct GpuVoxelMaterial {
    uvec4 face_texture_starts_0;
    uvec4 face_texture_starts_1;
    uvec4 face_texture_counts_0;
    uvec4 face_texture_counts_1;
    uvec4 flags_and_padding;
    vec4 base_color;
    vec4 surface_parameters;
    vec4 mapping_parameters;
    vec4 biome_transition;
    vec4 biome_tint;
    GpuTerrainSurfaceLayer surface_layers[9];
};

layout(std430, set = 0, binding = 1) readonly buffer VoxelMaterialTable {
    GpuVoxelMaterial materials[];
} voxel_material_table;

const uint MATERIAL_ALPHA_TESTED = 1U;
const uint MATERIAL_STABLE_ROTATIONS = 128U;
const uint MATERIAL_STABLE_MIRRORING = 256U;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} chunk;

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

uint face_value(uvec4 first, uvec4 second, uint face) {
    return face < 4U ? first[face] : second[face - 4U];
}

uint texture_variant_hash(ivec3 cell, uint face) {
    uint hash = 0x4f1bbcdcU ^ (face * 0x9e3779b9U);
    hash ^= uint(cell.x) * 0x85ebca6bU;
    hash ^= uint(cell.y) * 0xc2b2ae35U;
    hash ^= uint(cell.z) * 0x27d4eb2fU;
    hash ^= hash >> 16U;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15U;
    hash *= 0x846ca68bU;
    return hash ^ (hash >> 16U);
}

vec3 stable_periodic_position(vec3 local_position, uint coordinate_key) {
    uvec3 packed_coordinate =
        uvec3(coordinate_key & 0x3ffU,
              (coordinate_key >> 10U) & 0x3ffU,
              (coordinate_key >> 20U) & 0x3ffU);
    ivec3 chunk_coordinate = ivec3((packed_coordinate + 512U) & 0x3ffU) - ivec3(512);
    return vec3(chunk_coordinate * 32) + local_position;
}

vec2 world_mapped_uv(vec3 local_position, vec3 normal) {
    vec3 magnitude = abs(normal);
    if (magnitude.y >= magnitude.x && magnitude.y >= magnitude.z) {
        return vec2(local_position.x, normal.y > 0.0 ? -local_position.z
                                                     : local_position.z);
    }
    if (magnitude.x >= magnitude.z) {
        return vec2(normal.x > 0.0 ? -local_position.z : local_position.z,
                    local_position.y);
    }
    return vec2(normal.z > 0.0 ? local_position.x : -local_position.x,
                local_position.y);
}

vec2 orient_variant_uv(vec2 uv, uint hash, uint material_flags) {
    if ((material_flags & MATERIAL_STABLE_MIRRORING) != 0U && (hash & 4U) != 0U) {
        uv.x = 1.0 - uv.x;
    }
    if ((material_flags & MATERIAL_STABLE_ROTATIONS) != 0U) {
        uint rotation = hash & 3U;
        for (uint turn = 0U; turn < rotation; ++turn) {
            uv = vec2(uv.y, 1.0 - uv.x);
        }
    }
    return uv;
}

void main() {
    // Keep the shared terrain vertex interface fully consumed. These packed values reserve all
    // bits for valid terrain state, so this conjunction is never produced by the mesher.
    if (fragment_light == 0xffffffffU && fragment_state_bits == 0xffffffffU &&
        fragment_voxel_ao < 0.0 && fragment_lod_blend < 0.0 &&
        fragment_coordinate_key == 0xffffffffU) {
        discard;
    }
    GpuVoxelMaterial material =
        voxel_material_table.materials[min(fragment_voxel_type,
                                           uint(voxel_material_table.materials.length() - 1))];
    if ((material.flags_and_padding.x & MATERIAL_ALPHA_TESTED) == 0U) {
        return;
    }
    vec3 normal = normalize(fragment_normal);
    uint face = face_index(normal);
    uint start = face_value(material.face_texture_starts_0,
                            material.face_texture_starts_1, face);
    uint count = max(face_value(material.face_texture_counts_0,
                                material.face_texture_counts_1, face), 1U);
    vec3 local_position = fragment_world_position - chunk.camera_relative_origin.xyz;
    vec3 stable_position =
        stable_periodic_position(local_position,
                                 fragment_coordinate_key);
    uint hash =
        texture_variant_hash(ivec3(floor(stable_position - normal * 0.001)), face);
    uint layer = start + hash % count;
    vec2 uv = fract(world_mapped_uv(stable_position, normal) *
                    material.mapping_parameters.x);
    uv = orient_variant_uv(uv, hash, material.flags_and_padding.x);
    float alpha = texture(terrain_textures, vec3(uv, float(layer))).a *
                  material.base_color.a;
    if (alpha < 0.5) {
        discard;
    }
}
