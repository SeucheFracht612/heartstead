#version 450

layout(set = 0, binding = 0) uniform sampler2D input_hdr;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

vec3 bright(vec3 color) {
    float peak = max(color.r, max(color.g, color.b));
    return color * smoothstep(0.8, 1.6, peak);
}

void main() {
    vec2 texel = 1.0 / vec2(textureSize(input_hdr, 0));
    vec3 bloom = bright(texture(input_hdr, in_uv).rgb) * 0.28;
    bloom += bright(texture(input_hdr, in_uv + vec2(texel.x, 0.0)).rgb) * 0.12;
    bloom += bright(texture(input_hdr, in_uv - vec2(texel.x, 0.0)).rgb) * 0.12;
    bloom += bright(texture(input_hdr, in_uv + vec2(0.0, texel.y)).rgb) * 0.12;
    bloom += bright(texture(input_hdr, in_uv - vec2(0.0, texel.y)).rgb) * 0.12;
    bloom += bright(texture(input_hdr, in_uv + texel).rgb) * 0.06;
    bloom += bright(texture(input_hdr, in_uv - texel).rgb) * 0.06;
    bloom += bright(texture(input_hdr, in_uv + vec2(texel.x, -texel.y)).rgb) * 0.06;
    bloom += bright(texture(input_hdr, in_uv + vec2(-texel.x, texel.y)).rgb) * 0.06;
    out_color = vec4(bloom, 1.0);
}
