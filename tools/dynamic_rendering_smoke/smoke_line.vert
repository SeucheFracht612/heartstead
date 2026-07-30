#version 450

// Deliberately free of push constants and descriptors: the smoke test drives the deprecated
// mesh-draw path, which does not set push constants, so a shader that statically used them would
// trip VUID-vkCmdDraw-maintenance4-08602.
layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(location = 0) out vec4 fragment_color;

void main() {
    gl_Position = vec4(in_position, 1.0);
    fragment_color = in_color;
}
