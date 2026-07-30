#version 450

layout(set = 0, binding = 0) uniform sampler2DArray ui_atlas;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) flat in uint in_texture_layer;

layout(location = 0) out vec4 out_color;

vec3 linear_to_srgb(vec3 value) {
    vec3 low = value * 12.92;
    vec3 high = 1.055 * pow(max(value, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(value, vec3(0.0031308)));
}

void main() {
    vec4 color = texture(ui_atlas, vec3(in_uv, float(in_texture_layer))) * in_color;
    out_color = vec4(linear_to_srgb(max(color.rgb, vec3(0.0))), color.a);
}
