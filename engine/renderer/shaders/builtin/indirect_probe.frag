#version 450

layout(location = 0) in vec3 color;
layout(location = 0) out vec4 out_color;
layout(location = 1) out vec2 out_motion;

void main() {
    out_color = vec4(color, 1.0);
    out_motion = vec2(0.0);
}
