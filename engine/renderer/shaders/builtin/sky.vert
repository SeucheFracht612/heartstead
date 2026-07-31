#version 450

layout(location = 0) in vec2 in_position;
layout(location = 0) out vec2 fragment_ndc;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

void main() {
    gl_Position = vec4(in_position, 0.999, 1.0);
    fragment_ndc = in_position;
}
