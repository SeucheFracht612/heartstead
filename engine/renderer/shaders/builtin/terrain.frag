#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec2 fragment_uv;
layout(location = 2) flat in uint fragment_voxel_type;
layout(location = 3) flat in uint fragment_light;
layout(location = 4) flat in uint fragment_state_bits;
layout(location = 5) in vec3 fragment_world_position;
layout(location = 6) in float fragment_voxel_ao;

layout(set = 0, binding = 0) uniform sampler2DArray terrain_textures;
layout(set = 0, binding = 12) uniform sampler2DArray terrain_normal_textures;
layout(set = 0, binding = 13) uniform sampler2DArray terrain_surface_textures;

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

struct GpuLocalLight {
    vec4 position_radius;
    vec4 direction_kind;
    vec4 color_intensity;
    vec4 spot_shadow;
};

layout(std430, set = 0, binding = 2) readonly buffer LocalLights {
    GpuLocalLight lights[];
} local_lights;

layout(std430, set = 0, binding = 3) readonly buffer LightGrid {
    uint data[];
} light_grid;

layout(std430, set = 0, binding = 4) readonly buffer DirectionalShadowData {
    mat4 light_view_projection[4];
    vec4 split_distances;
    vec4 shadow_parameters;
    vec4 environment_parameters;
    vec4 camera_position;
    mat4 local_light_view_projection[2];
    vec4 local_shadow_parameters[2];
} shadows;

layout(set = 0, binding = 5) uniform sampler2DShadow shadow_cascade_0;
layout(set = 0, binding = 6) uniform sampler2DShadow shadow_cascade_1;
layout(set = 0, binding = 7) uniform sampler2DShadow shadow_cascade_2;
layout(set = 0, binding = 8) uniform sampler2DShadow shadow_cascade_3;
layout(set = 0, binding = 9) uniform samplerCube environment_map;
layout(set = 0, binding = 10) uniform sampler2DShadow local_shadow_0;
layout(set = 0, binding = 11) uniform sampler2DShadow local_shadow_1;

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
const uint MATERIAL_UNLIT = 32U;
const uint MATERIAL_FLUID = 64U;
const uint MATERIAL_STABLE_ROTATIONS = 128U;
const uint MATERIAL_STABLE_MIRRORING = 256U;
const uint NO_TEXTURE = 0xffffffffU;
const float PI = 3.14159265358979323846;

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

vec4 unpack_unorm8(uint packed) {
    return vec4(float(packed & 0xffU),
                float((packed >> 8U) & 0xffU),
                float((packed >> 16U) & 0xffU),
                float((packed >> 24U) & 0xffU)) / 255.0;
}

vec3 stable_periodic_position(vec3 local_position, uint coordinate_key) {
    uvec3 chunk_coordinate =
        uvec3(coordinate_key & 0x3ffU,
              (coordinate_key >> 10U) & 0x3ffU,
              (coordinate_key >> 20U) & 0x3ffU);
    return vec3(chunk_coordinate * 32U) + local_position;
}

uint periodic_lattice_hash(ivec3 coordinate, uint salt) {
    uvec3 periodic = uvec3(coordinate) & uvec3(0x7fffU);
    uint hash = salt ^ periodic.x * 0x85ebca6bU;
    hash ^= periodic.y * 0xc2b2ae35U;
    hash ^= periodic.z * 0x27d4eb2fU;
    hash ^= hash >> 16U;
    hash *= 0x7feb352dU;
    hash ^= hash >> 15U;
    hash *= 0x846ca68bU;
    return hash ^ (hash >> 16U);
}

float stable_value_noise(vec3 position, uint salt) {
    ivec3 cell = ivec3(floor(position));
    vec3 fraction = fract(position);
    fraction = fraction * fraction * (3.0 - 2.0 * fraction);
    float values[8];
    for (int z = 0; z < 2; ++z) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                int index = x + y * 2 + z * 4;
                values[index] =
                    float(periodic_lattice_hash(cell + ivec3(x, y, z), salt) & 0xffffU) /
                    65535.0;
            }
        }
    }
    float z0 = mix(mix(values[0], values[1], fraction.x),
                   mix(values[2], values[3], fraction.x), fraction.y);
    float z1 = mix(mix(values[4], values[5], fraction.x),
                   mix(values[6], values[7], fraction.x), fraction.y);
    return mix(z0, z1, fraction.z);
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

mat3 tangent_basis(vec3 normal) {
    vec3 magnitude = abs(normal);
    vec3 tangent;
    vec3 bitangent;
    if (magnitude.y >= magnitude.x && magnitude.y >= magnitude.z) {
        tangent = vec3(1.0, 0.0, 0.0);
        bitangent = vec3(0.0, 0.0, normal.y > 0.0 ? -1.0 : 1.0);
    } else if (magnitude.x >= magnitude.z) {
        tangent = vec3(0.0, 0.0, normal.x > 0.0 ? -1.0 : 1.0);
        bitangent = vec3(0.0, 1.0, 0.0);
    } else {
        tangent = vec3(normal.z > 0.0 ? 1.0 : -1.0, 0.0, 0.0);
        bitangent = vec3(0.0, 1.0, 0.0);
    }
    return mat3(tangent, bitangent, normal);
}

void orient_variant_uv(inout vec2 uv, inout vec2 tangent_normal, uint hash,
                       uint material_flags) {
    if ((material_flags & MATERIAL_STABLE_MIRRORING) != 0U && (hash & 4U) != 0U) {
        uv.x = 1.0 - uv.x;
        tangent_normal.x = -tangent_normal.x;
    }
    if ((material_flags & MATERIAL_STABLE_ROTATIONS) == 0U) {
        return;
    }
    uint rotation = hash & 3U;
    for (uint turn = 0U; turn < rotation; ++turn) {
        uv = vec2(uv.y, 1.0 - uv.x);
        tangent_normal = vec2(tangent_normal.y, -tangent_normal.x);
    }
}

vec3 fresnel_schlick(float cosine, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosine, 0.0, 1.0), 5.0);
}

// Split-sum environment BRDF integration fit. This is the shader-side equivalent of a compact
// BRDF LUT and preserves the roughness/NdotV response without another sampled resource.
vec2 environment_brdf(float roughness, float normal_view) {
    vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) +
             vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 =
        min(r.x * r.x, exp2(-9.28 * normal_view)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
}

vec3 evaluate_light(vec3 albedo, float metallic, float roughness, vec3 normal,
                    vec3 view_direction, vec3 light_direction, vec3 radiance) {
    vec3 halfway = normalize(view_direction + light_direction);
    float normal_light = max(dot(normal, light_direction), 0.0);
    float normal_view = max(dot(normal, view_direction), 0.0001);
    float normal_half = max(dot(normal, halfway), 0.0);
    float view_half = max(dot(view_direction, halfway), 0.0);
    float alpha = roughness * roughness;
    float alpha_squared = alpha * alpha;
    float denominator = normal_half * normal_half * (alpha_squared - 1.0) + 1.0;
    float distribution = alpha_squared / max(PI * denominator * denominator, 0.0001);
    float geometry_k = (roughness + 1.0) * (roughness + 1.0) / 8.0;
    float geometry_view = normal_view / (normal_view * (1.0 - geometry_k) + geometry_k);
    float geometry_light = normal_light / (normal_light * (1.0 - geometry_k) + geometry_k);
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = fresnel_schlick(view_half, f0);
    vec3 specular = distribution * geometry_view * geometry_light * fresnel /
                    max(4.0 * normal_view * normal_light, 0.0001);
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * normal_light;
}

float local_shadow_visibility(GpuLocalLight light, vec3 normal,
                              vec3 light_direction) {
    if (light.spot_shadow.z < 0.5) {
        return 1.0;
    }
    uint slot = min(uint(light.spot_shadow.z - 0.5), 1U);
    vec4 clip =
        shadows.local_light_view_projection[slot] *
        vec4(fragment_world_position, 1.0);
    if (clip.w <= 0.0) {
        return 1.0;
    }
    vec3 coordinate = clip.xyz / clip.w;
    coordinate.xy = coordinate.xy * 0.5 + 0.5;
    float bias = shadows.local_shadow_parameters[slot].x +
                 shadows.local_shadow_parameters[slot].y *
                     (1.0 - max(dot(normal, light_direction), 0.0));
    coordinate.z -= bias;
    if (any(lessThan(coordinate, vec3(0.0))) ||
        any(greaterThan(coordinate, vec3(1.0)))) {
        return 1.0;
    }
    vec2 texel = 1.0 / vec2(textureSize(local_shadow_0, 0));
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 sample_coordinate =
                vec3(coordinate.xy + vec2(x, y) * texel, coordinate.z);
            visibility += slot == 0U
                              ? texture(local_shadow_0, sample_coordinate)
                              : texture(local_shadow_1, sample_coordinate);
        }
    }
    return visibility / 9.0;
}

vec3 local_lighting(vec3 albedo, float metallic, float roughness, vec3 normal,
                    vec3 view_direction) {
    uint tiles_x = light_grid.data[0];
    uint tiles_y = light_grid.data[1];
    uint tile_size = light_grid.data[2];
    uint capacity = light_grid.data[3];
    uvec2 tile = min(uvec2(gl_FragCoord.xy) / max(tile_size, 1U),
                     uvec2(max(tiles_x, 1U) - 1U, max(tiles_y, 1U) - 1U));
    uint base = 4U + (tile.y * tiles_x + tile.x) * (capacity + 1U);
    uint count = min(light_grid.data[base], capacity);
    vec3 total = vec3(0.0);
    for (uint entry = 0U; entry < count; ++entry) {
        GpuLocalLight light = local_lights.lights[light_grid.data[base + 1U + entry]];
        vec3 to_light = light.position_radius.xyz - fragment_world_position;
        float distance_squared = dot(to_light, to_light);
        float distance = sqrt(max(distance_squared, 0.0001));
        if (distance >= light.position_radius.w) {
            continue;
        }
        vec3 direction = to_light / distance;
        float range = clamp(1.0 - distance / light.position_radius.w, 0.0, 1.0);
        float attenuation = range * range /
                            max(distance_squared, 1.0);
        if (uint(light.direction_kind.w + 0.5) == 2U) {
            float cone = dot(-direction, normalize(light.direction_kind.xyz));
            attenuation *= smoothstep(light.spot_shadow.y, light.spot_shadow.x, cone);
        }
        attenuation *= local_shadow_visibility(light, normal, direction);
        vec3 radiance = light.color_intensity.rgb * light.color_intensity.w * attenuation;
        total += evaluate_light(albedo, metallic, roughness, normal, view_direction,
                                direction, radiance);
    }
    return total;
}

vec3 local_tile_debug_color() {
    uint tiles_x = light_grid.data[0];
    uint tiles_y = light_grid.data[1];
    uint tile_size = max(light_grid.data[2], 1U);
    uint capacity = max(light_grid.data[3], 1U);
    uvec2 tile = min(uvec2(gl_FragCoord.xy) / tile_size,
                     uvec2(max(tiles_x, 1U) - 1U, max(tiles_y, 1U) - 1U));
    uint base = 4U + (tile.y * tiles_x + tile.x) * (capacity + 1U);
    float occupancy = float(min(light_grid.data[base], capacity)) / float(capacity);
    return mix(vec3(0.02, 0.08, 0.35), vec3(1.0, 0.1, 0.02), occupancy);
}

float sample_shadow_map(uint cascade, vec3 coordinate) {
    vec2 texel = 1.0 / vec2(textureSize(shadow_cascade_0, 0));
    float visibility = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 sample_coordinate =
                vec3(coordinate.xy + vec2(x, y) * texel, coordinate.z);
            if (cascade == 0U) {
                visibility += texture(shadow_cascade_0, sample_coordinate);
            } else if (cascade == 1U) {
                visibility += texture(shadow_cascade_1, sample_coordinate);
            } else if (cascade == 2U) {
                visibility += texture(shadow_cascade_2, sample_coordinate);
            } else {
                visibility += texture(shadow_cascade_3, sample_coordinate);
            }
        }
    }
    return visibility / 9.0;
}

float directional_shadow(vec3 normal, vec3 light_direction, out uint cascade) {
    float distance_to_camera =
        length(fragment_world_position - shadows.camera_position.xyz);
    cascade = distance_to_camera <= shadows.split_distances.x ? 0U :
              distance_to_camera <= shadows.split_distances.y ? 1U :
              distance_to_camera <= shadows.split_distances.z ? 2U : 3U;
    vec4 light_clip =
        shadows.light_view_projection[cascade] * vec4(fragment_world_position, 1.0);
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

void apply_surface_layers(GpuVoxelMaterial material, uint material_flags,
                          vec2 mapped_uv, vec3 stable_position, vec3 geometry_normal,
                          inout vec3 albedo, inout float roughness,
                          inout float metallic, inout float emissive) {
    if ((material_flags & MATERIAL_FLUID) != 0U) {
        return;
    }
    float shared_coverage = float((fragment_state_bits >> 9U) & 7U) / 7.0;
    if (shared_coverage <= 0.0) {
        return;
    }
    for (uint index = 0U; index < 9U; ++index) {
        if ((fragment_state_bits & (1U << index)) == 0U) {
            continue;
        }
        GpuTerrainSurfaceLayer layer = material.surface_layers[index];
        vec4 tint = unpack_unorm8(layer.packed.x);
        vec4 parameters = unpack_unorm8(layer.packed.y);
        float directional_coverage =
            (index == 1U || index == 2U)
                ? smoothstep(0.05, 0.72, geometry_normal.y)
                : (index == 4U ? smoothstep(-0.3, 0.65, geometry_normal.y) : 1.0);
        float requested =
            clamp(shared_coverage * parameters.x * directional_coverage, 0.0, 1.0);
        float noise = stable_value_noise(stable_position * (0.38 + float(index) * 0.027),
                                         0x68bc21ebU + index * 0x9e3779b9U);
        float mask = smoothstep(1.0 - requested - 0.18, 1.0 - requested + 0.18, noise);
        vec3 layer_color = tint.rgb;
        float layer_alpha = tint.a;
        if (layer.packed.z != NO_TEXTURE) {
            vec4 overlay =
                texture(terrain_textures, vec3(mapped_uv, float(layer.packed.z)));
            layer_color *= overlay.rgb;
            layer_alpha *= overlay.a;
        }
        float blend = clamp(mask * layer_alpha, 0.0, 1.0);
        albedo = mix(albedo, layer_color, blend);
        roughness = mix(roughness, parameters.y, blend);
        metallic = mix(metallic, parameters.z, blend);
        emissive += parameters.w * 4.0 * blend;
    }
}

void main() {
    uint table_length = uint(voxel_material_table.materials.length());
    uint material_index = min(fragment_voxel_type, max(table_length, 1U) - 1U);
    GpuVoxelMaterial material = voxel_material_table.materials[material_index];

    vec3 geometry_normal = normalize(fragment_normal);
    uint face = face_index(geometry_normal);
    uint texture_start =
        face_table_value(material.face_texture_starts_0, material.face_texture_starts_1, face);
    uint texture_count =
        max(face_table_value(material.face_texture_counts_0,
                             material.face_texture_counts_1, face),
            1U);
    uint coordinate_key = floatBitsToUint(chunk.camera_relative_origin.w);
    vec3 local_position = fragment_world_position - chunk.camera_relative_origin.xyz;
    vec3 stable_position = stable_periodic_position(local_position, coordinate_key);
    ivec3 stable_cell = ivec3(floor(stable_position - geometry_normal * 0.001));
    uint variant_hash = texture_variant_hash(stable_cell, face, 0x4f1bbcdcU);
    uint variant = variant_hash % texture_count;
    uint texture_layer = texture_start + variant;
    uint material_flags = material.flags_and_padding.x;
    vec2 mapped_uv =
        fract(world_mapped_uv(stable_position, geometry_normal) *
              material.mapping_parameters.x);
    vec2 orientation_placeholder = vec2(0.0);
    orient_variant_uv(mapped_uv, orientation_placeholder, variant_hash, material_flags);
    vec4 texel = texture(terrain_textures, vec3(mapped_uv, float(texture_layer)));
    vec3 tangent_normal =
        texture(terrain_normal_textures, vec3(mapped_uv, float(texture_layer))).xyz * 2.0 - 1.0;
    tangent_normal.xy *= material.mapping_parameters.y;
    tangent_normal = normalize(tangent_normal);
    vec2 normal_uv_placeholder = mapped_uv;
    vec2 tangent_xy = tangent_normal.xy;
    orient_variant_uv(normal_uv_placeholder, tangent_xy, variant_hash, material_flags);
    tangent_normal.xy = tangent_xy;
    vec3 normal = normalize(tangent_basis(geometry_normal) * tangent_normal);
    vec4 surface_data =
        texture(terrain_surface_textures, vec3(mapped_uv, float(texture_layer)));
    float surface_alpha = texel.a * material.base_color.a;
    if ((material_flags & MATERIAL_ALPHA_TESTED) != 0U && surface_alpha < 0.5) {
        discard;
    }

    // The authoritative field contains the maximum of propagated sky and emitted voxel light.
    // Keep unlit caves dark while retaining a small stylized readability floor.
    float normalized_voxel_light = float(fragment_light) / 255.0;
    float baked_light = 0.12 + 0.88 * pow(normalized_voxel_light, 1.25);
    float emissive = material.surface_parameters.x;
    float roughness =
        clamp(material.surface_parameters.y * surface_data.g, 0.045, 1.0);
    float material_occlusion =
        clamp(material.surface_parameters.z * surface_data.r * fragment_voxel_ao, 0.0, 1.0);
    float metallic = clamp(material.surface_parameters.w * surface_data.b, 0.0, 1.0);
    vec3 albedo = texel.rgb * material.base_color.rgb;
    float macro =
        stable_value_noise(stable_position / 24.0, 0xa511e9b3U);
    albedo *= 1.0 + (macro * 2.0 - 1.0) * material.mapping_parameters.z;
    albedo *= mix(vec3(1.0), material.biome_tint.rgb,
                  material.biome_transition.x * (0.65 + macro * 0.35));
    roughness = clamp(
        roughness + (macro * 2.0 - 1.0) * material.mapping_parameters.w, 0.045, 1.0);
    float transition_width = material.biome_transition.y;
    if (transition_width > 0.0) {
        float transition_noise =
            stable_value_noise(stable_position * material.biome_transition.w,
                               0x72e2a9d5U);
        float transition_signal =
            clamp(surface_data.a + (transition_noise * 2.0 - 1.0) * transition_width,
                  0.0, 1.0);
        float transition =
            smoothstep(0.5 - transition_width, 0.5 + transition_width,
                       transition_signal);
        transition = pow(clamp(transition, 0.0, 1.0),
                         material.biome_transition.z);
        albedo = mix(albedo * (0.88 + macro * 0.18), albedo,
                     transition * 0.55 + 0.45);
        roughness =
            clamp(roughness + (1.0 - transition) * 0.08, 0.045, 1.0);
    }
    apply_surface_layers(material, material_flags, mapped_uv, stable_position,
                         geometry_normal, albedo, roughness, metallic, emissive);
    vec3 view_direction =
        normalize(shadows.camera_position.xyz - fragment_world_position);
    vec3 light_direction = normalize(chunk.sun_direction_intensity.xyz);
    uint shadow_cascade = 0U;
    float shadow = directional_shadow(normal, light_direction, shadow_cascade);
    vec3 direct = evaluate_light(albedo, metallic, roughness, normal, view_direction,
                                 light_direction,
                                 vec3(chunk.sun_direction_intensity.w * shadow * PI));
    vec3 f0 = mix(vec3(0.04), albedo, metallic);
    float normal_view = max(dot(normal, view_direction), 0.0);
    vec2 integrated_brdf = environment_brdf(roughness, normal_view);
    vec3 reflection = reflect(-view_direction, normal);
    float rotation_sine = sin(shadows.environment_parameters.z);
    float rotation_cosine = cos(shadows.environment_parameters.z);
    reflection.xz = mat2(rotation_cosine, -rotation_sine,
                         rotation_sine, rotation_cosine) * reflection.xz;
    vec3 environment_specular =
        textureLod(environment_map, reflection, roughness * 5.0).rgb;
    vec3 environment_normal = normal;
    environment_normal.xz =
        mat2(rotation_cosine, -rotation_sine, rotation_sine, rotation_cosine) *
        environment_normal.xz;
    vec3 environment_diffuse =
        textureLod(environment_map, environment_normal, 5.0).rgb;
    vec3 environment =
        chunk.ambient_color_fog_start.rgb *
        (albedo * (1.0 - metallic) * environment_diffuse *
             shadows.environment_parameters.x +
         environment_specular *
             (f0 * integrated_brdf.x + integrated_brdf.y) *
             shadows.environment_parameters.y) *
        baked_light * material_occlusion;
    vec3 lit = environment + direct +
               local_lighting(albedo, metallic, roughness, normal, view_direction);
    vec3 color = (material_flags & MATERIAL_UNLIT) != 0U
                     ? albedo
                     : lit + albedo * emissive;
    uint debug_view = uint(shadows.shadow_parameters.w + 0.5);
    if (debug_view == 1U) {
        color = albedo;
    } else if (debug_view == 2U) {
        color = normal * 0.5 + 0.5;
    } else if (debug_view == 3U) {
        color = vec3(roughness);
    } else if (debug_view == 4U) {
        color = vec3(metallic);
    } else if (debug_view == 5U) {
        color = vec3(baked_light * material_occlusion);
    } else if (debug_view == 6U) {
        color = albedo * emissive;
    } else if (debug_view == 7U) {
        const vec3 cascade_colors[4] =
            vec3[4](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2),
                    vec3(0.2, 0.4, 1.0), vec3(1.0, 0.8, 0.2));
        color = cascade_colors[shadow_cascade] * mix(0.35, 1.0, shadow);
    } else if (debug_view == 8U) {
        color = local_tile_debug_color();
    } else if (debug_view == 9U || debug_view == 10U) {
        color = vec3(fract(fragment_uv), 0.0);
    } else if (debug_view == 11U) {
        vec3 tangent = normalize(abs(normal.y) > 0.9
                                     ? vec3(1.0, 0.0, 0.0)
                                     : cross(vec3(0.0, 1.0, 0.0), normal));
        color = tangent * 0.5 + 0.5;
    } else if (debug_view == 12U) {
        color = albedo;
    } else if (debug_view == 13U) {
        float lod = max(textureQueryLod(terrain_textures, fragment_uv).x, 0.0);
        color = vec3(fract(lod / 6.0), fract(lod / 3.0), fract(lod / 1.5));
    } else if (debug_view == 14U) {
        vec2 density =
            fwidth(fragment_uv) * vec2(textureSize(terrain_textures, 0).xy);
        float value =
            clamp(log2(max(length(density), 0.0001)) / 8.0 + 0.5, 0.0, 1.0);
        color = vec3(value, 1.0 - abs(value - 0.5) * 2.0, 1.0 - value);
    } else if (debug_view == 15U) {
        color = vec3(0.1, 0.9, 0.25);
    } else if (debug_view == 16U) {
        const vec3 lod_colors[4] =
            vec3[4](vec3(0.15, 0.85, 0.25), vec3(0.2, 0.5, 1.0),
                    vec3(1.0, 0.7, 0.15), vec3(1.0, 0.2, 0.3));
        color = lod_colors[(fragment_state_bits >> 28U) & 3U];
    } else if (debug_view == 19U) {
        color = vec3(0.0);
    } else if (debug_view == 20U) {
        color = vec3(1.0, 0.05, 0.5) *
                (0.2 + 0.8 * clamp(surface_alpha, 0.0, 1.0));
    }
    float fog_distance =
        length(fragment_world_position - shadows.camera_position.xyz);
    float fog = smoothstep(chunk.ambient_color_fog_start.w, chunk.fog_color_fog_end.w,
                           fog_distance);
    color = mix(color, chunk.fog_color_fog_end.rgb, fog);
    float output_alpha = (material_flags & MATERIAL_TRANSLUCENT) != 0U ? surface_alpha : 1.0;
    out_color = vec4(max(color, vec3(0.0)), output_alpha);
}
