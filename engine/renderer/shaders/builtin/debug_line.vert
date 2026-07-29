#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec4 fragment_color;

void main() {
    gl_Position = frame.view_projection * vec4(in_position, 1.0);
    fragment_color = in_color;
}
