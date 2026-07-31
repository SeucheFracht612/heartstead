#version 450

layout(location = 0) in vec2 fragment_ndc;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

float hash31(vec3 value) {
    value = fract(value * 0.1031);
    value += dot(value, value.yzx + 33.33);
    return fract((value.x + value.y) * value.z);
}

float value_noise(vec2 position) {
    vec2 cell = floor(position);
    vec2 blend = fract(position);
    blend = blend * blend * (3.0 - 2.0 * blend);
    float a = hash31(vec3(cell, 17.0));
    float b = hash31(vec3(cell + vec2(1.0, 0.0), 17.0));
    float c = hash31(vec3(cell + vec2(0.0, 1.0), 17.0));
    float d = hash31(vec3(cell + vec2(1.0), 17.0));
    return mix(mix(a, b, blend.x), mix(c, d, blend.x), blend.y);
}

float cloud_noise(vec2 position) {
    float result = 0.0;
    float amplitude = 0.58;
    for (int octave = 0; octave < 4; ++octave) {
        result += value_noise(position) * amplitude;
        position = position * 2.03 + vec2(13.7, 5.3);
        amplitude *= 0.48;
    }
    return result;
}

// Analytic full-screen atmosphere. Environment colors and the solar direction are authoritative;
// the scattering terms supply stable shape without requiring a sky texture.
void main() {
    mat4 inverse_view_projection = inverse(frame.view_projection);
    vec4 near_point = inverse_view_projection * vec4(fragment_ndc, 0.0, 1.0);
    vec4 far_point = inverse_view_projection * vec4(fragment_ndc, 1.0, 1.0);
    vec3 ray = normalize(far_point.xyz / far_point.w -
                         near_point.xyz / near_point.w);
    float height = clamp(ray.y * 0.5 + 0.5, 0.0, 1.0);
    float gradient = smoothstep(0.02, 0.86, height);
    vec3 horizon = frame.fog_color_fog_end.rgb;
    vec3 zenith = frame.ambient_color_fog_start.rgb;
    vec3 color = mix(horizon, zenith, gradient);

    vec3 sun_direction = normalize(frame.sun_direction_intensity.xyz);
    float sun_amount = clamp(frame.sun_direction_intensity.w, 0.0, 1.5);
    float ray_sun = max(dot(ray, sun_direction), 0.0);
    float rayleigh = 0.12 * (1.0 + ray_sun * ray_sun) *
                     pow(1.0 - max(ray.y, 0.0), 1.7);
    float mie = pow(ray_sun, 18.0) * 0.28;
    color += mix(horizon, vec3(1.0, 0.72, 0.42), 0.62) *
             (rayleigh + mie) * sun_amount;
    color += vec3(1.0, 0.78, 0.52) * pow(ray_sun, 720.0) *
             sun_amount * 9.0;

    float night = smoothstep(0.22, 0.0, sun_amount);
    float moon_alignment = max(dot(ray, -sun_direction), 0.0);
    color += vec3(0.42, 0.53, 0.78) * pow(moon_alignment, 950.0) *
             night * 2.6;
    vec3 star_cell = floor(ray * 780.0);
    float star_seed = hash31(star_cell);
    float star = step(0.9978, star_seed) *
                 pow(max(hash31(star_cell.yzx + 19.0), 0.0), 8.0);
    color += vec3(0.72, 0.80, 1.0) * star * night * smoothstep(0.42, 0.68, height) *
             3.0;

    float coverage = clamp(frame.camera_relative_origin.x, 0.0, 1.0);
    float density = clamp(frame.camera_relative_origin.y, 0.0, 1.0);
    float storm = clamp(frame.camera_relative_origin.w, 0.0, 1.0);
    if (coverage > 0.001 && ray.y > 0.015) {
        vec2 cloud_position =
            ray.xz / max(ray.y + 0.22, 0.12) * 0.72 +
            vec2(frame.camera_relative_origin.z * 0.008,
                 frame.camera_relative_origin.z * 0.003);
        float shape = cloud_noise(cloud_position);
        float threshold = mix(0.86, 0.34, coverage);
        float cloud = smoothstep(threshold, threshold + 0.18, shape) *
                      smoothstep(0.01, 0.18, ray.y) * density;
        vec3 cloud_lit = mix(vec3(0.52, 0.58, 0.64),
                             vec3(1.0, 0.95, 0.86), sun_amount);
        cloud_lit = mix(cloud_lit, vec3(0.10, 0.12, 0.16), storm * 0.78);
        color = mix(color, cloud_lit, cloud);
    }
    out_color = vec4(max(color, vec3(0.0)), 1.0);
}
