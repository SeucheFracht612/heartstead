#version 450

// Low is a deliberately separate, compile-time terrain path. It preserves texture selection,
// alpha testing, voxel lighting, directional shadows, fluids, fog, and the stable infinite-world
// mapping contract while omitting the expensive PBR, normal/surface maps, clustered lights,
// environment probes, procedural surface layers, and weather material modulation used by the
// higher presets.
layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec2 fragment_uv;
layout(location = 2) flat in uint fragment_voxel_type;
layout(location = 3) flat in uint fragment_light;
layout(location = 4) flat in uint fragment_state_bits;
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

layout(std430, set = 0, binding = 4) readonly buffer DirectionalShadowData {
    mat4 light_view_projection[4];
    vec4 split_distances;
    vec4 shadow_parameters;
    vec4 environment_parameters;
    vec4 camera_position;
    vec4 camera_forward;
    vec4 atmosphere_parameters;
    vec4 wind_parameters;
    vec4 weather_parameters;
    vec4 sky_zenith_cloud;
    vec4 sky_horizon_cloud;
    vec4 water_shallow_absorption;
    vec4 water_deep_scattering;
    vec4 water_scattering_refraction;
    vec4 water_foam_strength;
    vec4 water_parameters;
    mat4 local_light_view_projection[2];
    vec4 local_shadow_parameters[2];
} shadows;

layout(set = 0, binding = 5) uniform sampler2DShadow shadow_cascade_0;
layout(set = 0, binding = 6) uniform sampler2DShadow shadow_cascade_1;
layout(set = 0, binding = 7) uniform sampler2DShadow shadow_cascade_2;
layout(set = 0, binding = 8) uniform sampler2DShadow shadow_cascade_3;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_motion;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} chunk;

const uint MATERIAL_ALPHA_TESTED = 1U;
const uint MATERIAL_TRANSLUCENT = 2U;
const uint MATERIAL_UNLIT = 32U;
const uint MATERIAL_FLUID = 64U;
const uint MATERIAL_STABLE_ROTATIONS = 128U;
const uint MATERIAL_STABLE_MIRRORING = 256U;

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

uint texture_variant_hash(ivec3 local_cell, uint face) {
    uint hash = 0x4f1bbcdcU ^ (face * 0x9e3779b9U);
    hash ^= uint(local_cell.x) * 0x85ebca6bU;
    hash ^= uint(local_cell.y) * 0xc2b2ae35U;
    hash ^= uint(local_cell.z) * 0x27d4eb2fU;
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

float sample_shadow_map(uint cascade, vec3 coordinate) {
    if (cascade == 0U) {
        return texture(shadow_cascade_0, coordinate);
    }
    if (cascade == 1U) {
        return texture(shadow_cascade_1, coordinate);
    }
    if (cascade == 2U) {
        return texture(shadow_cascade_2, coordinate);
    }
    return texture(shadow_cascade_3, coordinate);
}

float directional_shadow(vec3 normal, vec3 light_direction, out uint cascade) {
    float distance_to_camera =
        max(dot(fragment_world_position - shadows.camera_position.xyz,
                shadows.camera_forward.xyz), 0.0);
    cascade = distance_to_camera <= shadows.split_distances.x ? 0U :
              distance_to_camera <= shadows.split_distances.y ? 1U :
              distance_to_camera <= shadows.split_distances.z ? 2U : 3U;
    vec4 light_clip =
        shadows.light_view_projection[cascade] * vec4(fragment_world_position, 1.0);
    if (light_clip.w <= 0.0) {
        return 1.0;
    }
    vec3 coordinate = light_clip.xyz / light_clip.w;
    coordinate.xy = coordinate.xy * 0.5 + 0.5;
    float bias = shadows.shadow_parameters.x +
                 shadows.shadow_parameters.y *
                     (1.0 - max(dot(normal, light_direction), 0.0));
    coordinate.z -= bias;
    if (any(lessThan(coordinate, vec3(0.0))) ||
        any(greaterThan(coordinate, vec3(1.0)))) {
        return 1.0;
    }
    float visibility = sample_shadow_map(cascade, coordinate);
    float fade_start = shadows.split_distances.w *
                       (1.0 - shadows.shadow_parameters.z);
    float fade = smoothstep(fade_start, shadows.split_distances.w, distance_to_camera);
    return mix(visibility, 1.0, fade);
}

void main() {
    if (fragment_lod_blend < 0.999) {
        uvec2 dither_cell = uvec2(floor(fract(fragment_uv) * 256.0));
        uint dither_hash = dither_cell.x * 0x9e3779b9U ^ dither_cell.y * 0x85ebca6bU;
        dither_hash ^= dither_hash >> 16U;
        if (float(dither_hash & 0xffffU) / 65535.0 > fragment_lod_blend) {
            discard;
        }
    }

    uint table_length = uint(voxel_material_table.materials.length());
    uint material_index = min(fragment_voxel_type, max(table_length, 1U) - 1U);
    GpuVoxelMaterial material = voxel_material_table.materials[material_index];
    uint material_flags = material.flags_and_padding.x;
    vec3 normal = normalize(fragment_normal);
    uint face = face_index(normal);
    uint texture_start =
        face_table_value(material.face_texture_starts_0, material.face_texture_starts_1, face);
    uint texture_count =
        max(face_table_value(material.face_texture_counts_0,
                             material.face_texture_counts_1, face), 1U);

    vec3 local_position = fragment_world_position - chunk.camera_relative_origin.xyz;
    vec3 stable_position = stable_periodic_position(local_position, fragment_coordinate_key);
    uint variant_hash =
        texture_variant_hash(ivec3(floor(stable_position - normal * 0.001)), face);
    uint texture_layer = texture_start + variant_hash % texture_count;
    bool fluid = (material_flags & MATERIAL_FLUID) != 0U;
    vec2 mapped_uv = fract(world_mapped_uv(stable_position, normal) *
                           material.mapping_parameters.x);
    if (fluid) {
        mapped_uv = fract(mapped_uv + vec2(0.0, shadows.atmosphere_parameters.x *
                                                    shadows.water_parameters.y));
    }
    mapped_uv = orient_variant_uv(mapped_uv, variant_hash, material_flags);
    vec4 texel = texture(terrain_textures, vec3(mapped_uv, float(texture_layer)));
    float surface_alpha = texel.a * material.base_color.a;
    if ((material_flags & MATERIAL_ALPHA_TESTED) != 0U && surface_alpha < 0.5) {
        discard;
    }

    vec3 albedo = texel.rgb * material.base_color.rgb;
    albedo *= mix(vec3(1.0), material.biome_tint.rgb, material.biome_transition.x);
    float normalized_voxel_light = float(fragment_light) / 255.0;
    float baked_light = (0.16 + 0.84 * normalized_voxel_light) * fragment_voxel_ao;
    vec3 light_direction = normalize(chunk.sun_direction_intensity.xyz);
    uint shadow_cascade = 0U;
    float shadow = directional_shadow(normal, light_direction, shadow_cascade);
    float diffuse = max(dot(normal, light_direction), 0.0);
    vec3 ambient = albedo * chunk.ambient_color_fog_start.rgb * baked_light;
    vec3 direct = albedo * chunk.sun_direction_intensity.w * diffuse * shadow;
    vec3 color = (material_flags & MATERIAL_UNLIT) != 0U
                     ? albedo
                     : ambient + direct + albedo * material.surface_parameters.x;

    if (fluid) {
        float amount = float(fragment_state_bits & 0x0fU) / 8.0;
        color = mix(color, shadows.water_deep_scattering.rgb,
                    clamp((1.0 - amount) * 0.35, 0.0, 0.35));
        surface_alpha = clamp(mix(0.42, 0.78, amount), 0.0, 0.9);
    }

    uint debug_view = uint(shadows.shadow_parameters.w + 0.5);
    if (debug_view == 1U || debug_view == 12U) {
        color = albedo;
    } else if (debug_view == 2U) {
        color = normal * 0.5 + 0.5;
    } else if (debug_view == 5U) {
        color = vec3(baked_light);
    } else if (debug_view == 7U) {
        const vec3 cascade_colors[4] =
            vec3[4](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2),
                    vec3(0.2, 0.4, 1.0), vec3(1.0, 0.8, 0.2));
        color = cascade_colors[shadow_cascade] * mix(0.35, 1.0, shadow);
    } else if (debug_view == 9U || debug_view == 10U) {
        color = vec3(fract(fragment_uv), 0.0);
    } else if (debug_view == 16U) {
        const vec3 lod_colors[4] =
            vec3[4](vec3(0.15, 0.85, 0.25), vec3(0.2, 0.5, 1.0),
                    vec3(1.0, 0.7, 0.15), vec3(1.0, 0.2, 0.3));
        color = lod_colors[(fragment_state_bits >> 28U) & 3U];
    }

    float fog_distance = length(fragment_world_position - shadows.camera_position.xyz);
    float fog = smoothstep(chunk.ambient_color_fog_start.w, chunk.fog_color_fog_end.w,
                           fog_distance);
    color = mix(color, chunk.fog_color_fog_end.rgb, fog);
    float output_alpha =
        (material_flags & MATERIAL_TRANSLUCENT) != 0U || fluid ? surface_alpha : 1.0;
    out_color = vec4(max(color, vec3(0.0)), output_alpha);
    out_motion = vec4(0.0, 0.0, 0.0, output_alpha);
}
