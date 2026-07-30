#version 450

layout(location = 0) in vec4 fragment_color;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 value) {
    vec3 low = value * 12.92;
    vec3 high = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(value, vec3(0.0031308)));
}

void main() {
    out_color = vec4(linear_to_srgb(max(fragment_color.rgb, vec3(0.0))),
                     fragment_color.a);
}
