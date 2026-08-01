#version 450

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec3 color;

void main() {
    const vec2 positions[3] = vec2[3](vec2(-0.7, -0.6), vec2(0.7, -0.6), vec2(0.0, 0.7));
    gl_Position = vec4(positions[gl_VertexIndex], 0.5, 1.0);
    color = vec3(0.2, 0.8, 0.3);
}
