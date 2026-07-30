#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_hdr;
layout(set = 0, binding = 1) uniform sampler2D scene_ao;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

void main() {
    vec3 color = texture(scene_hdr, in_uv).rgb;
    float occlusion = texture(scene_ao, in_uv).r;
    out_color = vec4(color * mix(0.72, 1.0, occlusion), 1.0);
}
