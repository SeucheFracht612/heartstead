#version 450

layout(location = 0) in vec4 fragment_color;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec4 out_color;
layout(location = 1) out vec4 out_motion;

// Writes linear radiance into the HDR scene target. Display encoding happens once, in the
// tone mapping pass; a transfer function applied here would be applied twice.
void main() {
    out_color = vec4(max(fragment_color.rgb, vec3(0.0)),
                     fragment_color.a);
    out_motion = vec4(0.0, 0.0, 0.0, fragment_color.a);
}
