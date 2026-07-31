#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec4 fragment_tangent;
layout(location = 2) in vec3 fragment_world_position;
layout(location = 3) in vec2 fragment_uv0;
layout(location = 4) in vec2 fragment_uv1;
layout(location = 5) in vec4 fragment_color;
layout(location = 6) flat in uint fragment_layer;
layout(location = 7) flat in uint fragment_material;
layout(location = 8) in vec4 fragment_skin_weights;

layout(set = 0, binding = 2) uniform sampler2DArray surface_textures;
layout(set = 0, binding = 4) uniform sampler2DArray surface_data_textures;

struct GpuTextureBinding {
    uvec4 metadata;
    vec4 transform;
};

struct GpuSurfaceMaterial {
    GpuTextureBinding textures[5];
    vec4 base_color;
    vec4 emissive_metallic;
    vec4 roughness_normal_occlusion_alpha;
    uvec4 flags_and_padding;
};

layout(std430, set = 0, binding = 3) readonly buffer SurfaceMaterials {
    GpuSurfaceMaterial materials[];
} surface_materials;

struct GpuLocalLight {
    vec4 position_radius;
    vec4 direction_kind;
    vec4 color_intensity;
    vec4 spot_shadow;
};

layout(std430, set = 0, binding = 7) readonly buffer LocalLights {
    GpuLocalLight lights[];
} local_lights;

layout(std430, set = 0, binding = 8) readonly buffer LightGrid {
    uint data[];
} light_grid;

layout(std430, set = 0, binding = 9) readonly buffer DirectionalShadowData {
    mat4 light_view_projection[4];
    vec4 split_distances;
    vec4 shadow_parameters;
    vec4 environment_parameters;
    vec4 camera_position;
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

layout(set = 0, binding = 10) uniform sampler2DShadow shadow_cascade_0;
layout(set = 0, binding = 11) uniform sampler2DShadow shadow_cascade_1;
layout(set = 0, binding = 12) uniform sampler2DShadow shadow_cascade_2;
layout(set = 0, binding = 13) uniform sampler2DShadow shadow_cascade_3;
layout(set = 0, binding = 14) uniform samplerCube environment_map;
layout(set = 0, binding = 15) uniform sampler2DShadow local_shadow_0;
layout(set = 0, binding = 16) uniform sampler2DShadow local_shadow_1;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec4 out_color;

const uint LAYER_TRANSPARENT = 2U;
const uint MATERIAL_ALPHA_TESTED = 1U;
const uint MATERIAL_TWO_SIDED = 8U;
const uint MATERIAL_UNLIT = 32U;
const float PI = 3.14159265358979323846;

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
    vec3 fresnel_base = mix(vec3(0.04), albedo, metallic);
    vec3 fresnel = fresnel_base + (1.0 - fresnel_base) * pow(1.0 - view_half, 5.0);
    vec3 specular = distribution * geometry_view * geometry_light * fresnel /
                    max(4.0 * normal_view * normal_light, 0.0001);
    vec3 diffuse = (1.0 - fresnel) * (1.0 - metallic) * albedo / PI;
    return (diffuse + specular) * radiance * normal_light;
}

vec2 environment_brdf(float roughness, float normal_view) {
    vec4 r = roughness * vec4(-1.0, -0.0275, -0.572, 0.022) +
             vec4(1.0, 0.0425, 1.04, -0.04);
    float a004 =
        min(r.x * r.x, exp2(-9.28 * normal_view)) * r.x + r.y;
    return vec2(-1.04, 1.04) * a004 + r.zw;
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

vec3 evaluate_local_lights(vec3 albedo, float metallic, float roughness, vec3 normal,
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
        float attenuation = range * range / max(distance_squared, 1.0);
        if (uint(light.direction_kind.w + 0.5) == 2U) {
            float cone = dot(-direction, normalize(light.direction_kind.xyz));
            attenuation *= smoothstep(light.spot_shadow.y, light.spot_shadow.x, cone);
        }
        attenuation *= local_shadow_visibility(light, normal, direction);
        total += evaluate_light(albedo, metallic, roughness, normal, view_direction,
                                direction,
                                light.color_intensity.rgb * light.color_intensity.w *
                                    attenuation);
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
    return mix(visibility, 1.0,
               smoothstep(fade_start, shadows.split_distances.w,
                          distance_to_camera));
}

vec2 binding_uv(GpuTextureBinding binding) {
    vec2 uv = binding.metadata.z == 0U ? fragment_uv0 : fragment_uv1;
    uv *= binding.transform.zw;
    float rotation = uintBitsToFloat(binding.metadata.w);
    float sine = sin(rotation);
    float cosine = cos(rotation);
    return binding.transform.xy +
           mat2(cosine, sine, -sine, cosine) * uv;
}

int wrapped_coordinate(int coordinate, int extent, uint mode) {
    if (mode == 1U) {
        return clamp(coordinate, 0, extent - 1);
    }
    int repeated = coordinate % extent;
    if (repeated < 0) {
        repeated += extent;
    }
    if (mode == 0U) {
        return repeated;
    }
    int mirrored = coordinate % (extent * 2);
    if (mirrored < 0) {
        mirrored += extent * 2;
    }
    return mirrored < extent ? mirrored : extent * 2 - 1 - mirrored;
}

vec4 fetch_wrapped(sampler2DArray image, uint layer, ivec2 coordinate, int level,
                   uint sampler_state) {
    ivec2 extent = textureSize(image, level).xy;
    uint wrap_s = (sampler_state >> 4U) & 3U;
    uint wrap_t = (sampler_state >> 6U) & 3U;
    ivec2 wrapped = ivec2(wrapped_coordinate(coordinate.x, extent.x, wrap_s),
                          wrapped_coordinate(coordinate.y, extent.y, wrap_t));
    return texelFetch(image, ivec3(wrapped, int(layer)), level);
}

vec4 sample_level(sampler2DArray image, uint layer, vec2 uv, int level,
                  uint sampler_state, bool linear_filter) {
    ivec2 extent = textureSize(image, level).xy;
    vec2 position = uv * vec2(extent) - vec2(0.5);
    ivec2 first = ivec2(floor(position));
    if (!linear_filter) {
        return fetch_wrapped(image, layer, ivec2(floor(position + vec2(0.5))), level,
                             sampler_state);
    }
    vec2 fraction = fract(position);
    vec4 top = mix(fetch_wrapped(image, layer, first, level, sampler_state),
                   fetch_wrapped(image, layer, first + ivec2(1, 0), level, sampler_state),
                   fraction.x);
    vec4 bottom =
        mix(fetch_wrapped(image, layer, first + ivec2(0, 1), level, sampler_state),
            fetch_wrapped(image, layer, first + ivec2(1, 1), level, sampler_state),
            fraction.x);
    return mix(top, bottom, fraction.y);
}

vec4 sample_binding(sampler2DArray image, GpuTextureBinding binding) {
    vec2 uv = binding_uv(binding);
    uint sampler_state = binding.metadata.y;
    uint layer = binding.metadata.x;
    // Array layers do not participate in implicit derivatives, so the query
    // takes the two-dimensional texture coordinate.
    float lod = max(textureQueryLod(image, uv).x, 0.0);
    int maximum_level = textureQueryLevels(image) - 1;
    if (lod <= 0.0) {
        bool linear_filter = (sampler_state & 1U) != 0U;
        return sample_level(image, layer, uv, 0, sampler_state, linear_filter);
    }
    uint min_filter = (sampler_state >> 1U) & 7U;
    if (min_filter <= 1U) {
        return sample_level(image, layer, uv, 0, sampler_state, min_filter == 1U);
    }
    bool linear_filter = min_filter == 3U || min_filter == 5U;
    float clamped_lod = clamp(lod, 0.0, float(maximum_level));
    if (min_filter == 2U || min_filter == 3U) {
        int level = int(floor(clamped_lod + 0.5));
        return sample_level(image, layer, uv, level, sampler_state, linear_filter);
    }
    int first_level = int(floor(clamped_lod));
    int second_level = min(first_level + 1, maximum_level);
    vec4 first = sample_level(image, layer, uv, first_level, sampler_state,
                              linear_filter);
    vec4 second = sample_level(image, layer, uv, second_level, sampler_state,
                               linear_filter);
    return mix(first, second, fract(clamped_lod));
}

// Writes linear radiance into the HDR scene target. Display encoding happens once, in the
// tone mapping pass; a transfer function applied here would be applied twice.
void main() {
    GpuSurfaceMaterial material = surface_materials.materials[fragment_material];
    vec4 base_color =
        sample_binding(surface_textures, material.textures[0]) *
        material.base_color * fragment_color;
    const uint material_flags = material.flags_and_padding.x;
    if ((material_flags & MATERIAL_ALPHA_TESTED) != 0U &&
        base_color.a < material.roughness_normal_occlusion_alpha.w) {
        discard;
    }
    vec3 emissive =
        sample_binding(surface_textures, material.textures[4]).rgb *
        material.emissive_metallic.rgb;
    vec3 color = base_color.rgb + emissive;
    if ((material_flags & MATERIAL_UNLIT) == 0U) {
        vec3 normal = normalize(fragment_normal);
        if ((material_flags & MATERIAL_TWO_SIDED) != 0U && !gl_FrontFacing) {
            normal = -normal;
        }
        vec3 tangent = normalize(fragment_tangent.xyz -
                                 normal * dot(normal, fragment_tangent.xyz));
        vec3 bitangent = normalize(cross(normal, tangent)) * fragment_tangent.w;
        vec3 tangent_normal =
            sample_binding(surface_data_textures, material.textures[2]).xyz *
                2.0 -
            1.0;
        tangent_normal.xy *= material.roughness_normal_occlusion_alpha.y;
        normal = normalize(mat3(tangent, bitangent, normal) * tangent_normal);

        vec4 metallic_roughness =
            sample_binding(surface_data_textures, material.textures[1]);
        float metallic =
            clamp(material.emissive_metallic.w * metallic_roughness.b, 0.0, 1.0);
        float roughness =
            clamp(material.roughness_normal_occlusion_alpha.x *
                      metallic_roughness.g,
                  0.045, 1.0);
        float occlusion = mix(
            1.0,
            sample_binding(surface_data_textures, material.textures[3]).r,
            material.roughness_normal_occlusion_alpha.z);

        vec3 view_direction =
            normalize(shadows.camera_position.xyz - fragment_world_position);
        vec3 light_direction = normalize(frame.sun_direction_intensity.xyz);
        uint shadow_cascade = 0U;
        float shadow = directional_shadow(normal, light_direction, shadow_cascade);
        float normal_view = max(dot(normal, view_direction), 0.0001);
        vec3 fresnel_base = mix(vec3(0.04), base_color.rgb, metallic);
        vec3 direct =
            evaluate_light(base_color.rgb, metallic, roughness, normal, view_direction,
                           light_direction,
                           vec3(frame.sun_direction_intensity.w * shadow * PI));
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
        vec3 ambient =
            frame.ambient_color_fog_start.rgb *
            (base_color.rgb * (1.0 - metallic) * environment_diffuse *
                 shadows.environment_parameters.x +
             environment_specular *
                 (fresnel_base * integrated_brdf.x + integrated_brdf.y) *
                 shadows.environment_parameters.y) *
            occlusion * mix(0.08, 1.0, shadow);
        color = ambient + direct +
                evaluate_local_lights(base_color.rgb, metallic, roughness, normal,
                                      view_direction) +
                emissive;
        uint debug_view = uint(shadows.shadow_parameters.w + 0.5);
        if (debug_view == 1U) {
            color = base_color.rgb;
        } else if (debug_view == 2U) {
            color = normal * 0.5 + 0.5;
        } else if (debug_view == 3U) {
            color = vec3(roughness);
        } else if (debug_view == 4U) {
            color = vec3(metallic);
        } else if (debug_view == 5U) {
            color = vec3(occlusion);
        } else if (debug_view == 6U) {
            color = emissive;
        } else if (debug_view == 7U) {
            const vec3 cascade_colors[4] =
                vec3[4](vec3(1.0, 0.2, 0.2), vec3(0.2, 1.0, 0.2),
                        vec3(0.2, 0.4, 1.0), vec3(1.0, 0.8, 0.2));
            color = cascade_colors[shadow_cascade] * mix(0.35, 1.0, shadow);
        } else if (debug_view == 8U) {
            color = local_tile_debug_color();
        } else if (debug_view == 9U) {
            color = vec3(fract(fragment_uv0), 0.0);
        } else if (debug_view == 10U) {
            color = vec3(fract(fragment_uv1), 0.0);
        } else if (debug_view == 11U) {
            color = fragment_tangent.xyz * 0.5 + 0.5;
        } else if (debug_view == 12U) {
            color = fragment_color.rgb;
        } else if (debug_view == 13U) {
            float lod =
                max(textureQueryLod(surface_textures,
                                    binding_uv(material.textures[0])).x,
                    0.0);
            color = vec3(fract(lod / 6.0), fract(lod / 3.0), fract(lod / 1.5));
        } else if (debug_view == 14U) {
            vec2 density = fwidth(fragment_uv0) *
                           vec2(textureSize(surface_textures, 0).xy);
            float value = clamp(log2(max(length(density), 0.0001)) / 8.0 + 0.5,
                                0.0, 1.0);
            color = vec3(value, 1.0 - abs(value - 0.5) * 2.0, 1.0 - value);
        } else if (debug_view == 15U) {
            color = vec3(0.1, 0.9, 0.25);
        } else if (debug_view == 16U) {
            const vec3 lod_colors[4] =
                vec3[4](vec3(0.15, 0.85, 0.25), vec3(0.2, 0.5, 1.0),
                        vec3(1.0, 0.7, 0.15), vec3(1.0, 0.2, 0.3));
            color = lod_colors[fragment_material & 3U];
        } else if (debug_view == 19U) {
            color = fragment_skin_weights.rgb +
                    vec3(fragment_skin_weights.a * 0.5);
        } else if (debug_view == 20U) {
            color = vec3(1.0, 0.05, 0.5) *
                    (0.2 + 0.8 * clamp(base_color.a, 0.0, 1.0));
        }
    }
    float fog = smoothstep(frame.ambient_color_fog_start.w,
                           frame.fog_color_fog_end.w,
                           length(fragment_world_position -
                                  shadows.camera_position.xyz));
    float fog_distance =
        length(fragment_world_position - shadows.camera_position.xyz);
    float height_density =
        shadows.atmosphere_parameters.y *
        exp(-max(fragment_world_position.y - shadows.camera_position.y, 0.0) *
            shadows.atmosphere_parameters.z);
    float volumetric_fog =
        1.0 - exp(-fog_distance *
                  (height_density + shadows.atmosphere_parameters.w * 0.0008));
    fog = max(fog, clamp(volumetric_fog, 0.0, 1.0));
    color = mix(color, frame.fog_color_fog_end.rgb, fog);
    float alpha = fragment_layer == LAYER_TRANSPARENT ? base_color.a : 1.0;
    out_color = vec4(max(color, vec3(0.0)), alpha);
}
