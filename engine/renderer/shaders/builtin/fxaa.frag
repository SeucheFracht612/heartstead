#version 450

layout(set = 0, binding = 0) uniform sampler2D input_hdr;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

float luma(vec3 color) {
    return dot(color, vec3(0.2126, 0.7152, 0.0722));
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(input_hdr, 0));
    vec3 center = texture(input_hdr, in_uv).rgb;
    float north = luma(texture(input_hdr, in_uv + vec2(0.0, -texel.y)).rgb);
    float south = luma(texture(input_hdr, in_uv + vec2(0.0, texel.y)).rgb);
    float west = luma(texture(input_hdr, in_uv + vec2(-texel.x, 0.0)).rgb);
    float east = luma(texture(input_hdr, in_uv + vec2(texel.x, 0.0)).rgb);
    float center_luma = luma(center);
    float minimum = min(center_luma, min(min(north, south), min(west, east)));
    float maximum = max(center_luma, max(max(north, south), max(west, east)));
    if (maximum - minimum < max(0.0312, maximum * 0.125)) {
        out_color = vec4(center, 1.0);
        return;
    }
    vec2 direction = vec2(-(north - south), west - east);
    float reduction = max((north + south + west + east) * 0.03125, 0.0001);
    direction = clamp(direction / (min(abs(direction.x), abs(direction.y)) + reduction),
                      vec2(-8.0), vec2(8.0)) * texel;
    vec3 first = 0.5 *
                 (texture(input_hdr, in_uv + direction * (1.0 / 3.0 - 0.5)).rgb +
                  texture(input_hdr, in_uv + direction * (2.0 / 3.0 - 0.5)).rgb);
    vec3 second = first * 0.5 +
                  0.25 * (texture(input_hdr, in_uv + direction * -0.5).rgb +
                          texture(input_hdr, in_uv + direction * 0.5).rgb);
    float second_luma = luma(second);
    out_color = vec4(second_luma < minimum || second_luma > maximum ? first : second, 1.0);
}
