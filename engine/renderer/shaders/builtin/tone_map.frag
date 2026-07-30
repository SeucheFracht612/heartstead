#version 450

// Resolves the linear HDR scene target to the display format. This is the only place in the
// renderer that applies exposure, a tone mapping curve, or a display transfer function. World
// shaders write linear radiance and must not encode sRGB themselves.

layout(set = 0, binding = 0) uniform sampler2D scene_hdr;
layout(set = 0, binding = 1) uniform sampler2D bloom_hdr;

layout(push_constant) uniform ToneMapPushConstants {
    float exposure_scale;
    float white_point;
    uint tone_mapping;
    uint padding;
    float saturation;
    float contrast;
    float bloom_intensity;
    float grading_padding;
} tone_map;

layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

const uint TONE_MAPPING_NONE = 0U;
const uint TONE_MAPPING_REINHARD = 1U;
const uint TONE_MAPPING_ACES_APPROX = 2U;
const uint TONE_MAPPING_KHRONOS_PBR_NEUTRAL = 3U;

vec3 reinhard(vec3 color) {
    return color / (1.0 + color);
}

// Narkowicz 2015, "ACES Filmic Tone Mapping Curve".
vec3 aces_approx(vec3 color) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((color * (a * color + b)) / (color * (c * color + d) + e), 0.0, 1.0);
}

// Khronos PBR neutral tone mapper. Keeps saturated albedo closer to its authored hue than the
// ACES fit does, which matters for the stylized voxel palette.
vec3 khronos_pbr_neutral(vec3 color) {
    const float start_compression = 0.8 - 0.04;
    const float desaturation = 0.15;

    float x = min(color.r, min(color.g, color.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    color -= offset;

    float peak = max(color.r, max(color.g, color.b));
    if (peak < start_compression) {
        return color;
    }

    float d = 1.0 - start_compression;
    float new_peak = 1.0 - d * d / (peak + d - start_compression);
    color *= new_peak / peak;

    float g = 1.0 - 1.0 / (desaturation * (peak - new_peak) + 1.0);
    return mix(color, vec3(new_peak), g);
}

vec3 apply_tone_mapping(vec3 color) {
    if (tone_map.tone_mapping == TONE_MAPPING_REINHARD) {
        return reinhard(color);
    }
    if (tone_map.tone_mapping == TONE_MAPPING_ACES_APPROX) {
        return aces_approx(color);
    }
    if (tone_map.tone_mapping == TONE_MAPPING_KHRONOS_PBR_NEUTRAL) {
        return khronos_pbr_neutral(color);
    }
    return clamp(color, 0.0, 1.0);
}

// The swapchain is created with a UNORM format, so the display transfer function is applied here.
// Switching the swapchain to an _SRGB format means deleting this call, not moving it.
vec3 linear_to_srgb(vec3 value) {
    vec3 low = value * 12.92;
    vec3 high = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(value, vec3(0.0031308)));
}

void main() {
    vec3 radiance = texture(scene_hdr, in_uv).rgb +
                    texture(bloom_hdr, in_uv).rgb * tone_map.bloom_intensity;
    radiance = max(radiance, vec3(0.0)) * tone_map.exposure_scale;
    radiance /= max(tone_map.white_point, 1.0e-4);
    vec3 display = clamp(apply_tone_mapping(radiance), 0.0, 1.0);
    float luminance = dot(display, vec3(0.2126, 0.7152, 0.0722));
    display = mix(vec3(luminance), display, tone_map.saturation);
    display = (display - 0.5) * tone_map.contrast + 0.5;
    display = clamp(display, 0.0, 1.0);
    out_color = vec4(linear_to_srgb(display), 1.0);
}
