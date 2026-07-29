#version 450

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec2 in_uv;
layout(location = 2) in vec4 in_color;
layout(location = 3) in uint in_texture_layer;

layout(location = 0) out vec2 out_uv;
layout(location = 1) out vec4 out_color;
layout(location = 2) flat out uint out_texture_layer;

void main() {
    gl_Position = vec4(in_position, 0.0, 1.0);
    out_uv = in_uv;
    out_color = in_color;
    out_texture_layer = in_texture_layer;
}
