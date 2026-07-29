#version 450

layout(set = 0, binding = 0) uniform sampler2DArray ui_atlas;

layout(location = 0) in vec2 in_uv;
layout(location = 1) in vec4 in_color;
layout(location = 2) flat in uint in_texture_layer;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = texture(ui_atlas, vec3(in_uv, float(in_texture_layer))) * in_color;
}
