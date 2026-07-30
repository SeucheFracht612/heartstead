#version 450

layout(location = 0) in float fragment_height;
layout(location = 0) out vec4 out_color;

layout(push_constant) uniform ChunkPushConstants {
    mat4 view_projection;
    vec4 camera_relative_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

// Writes linear radiance into the HDR scene target. Display encoding happens once, in the
// tone mapping pass; a transfer function applied here would be applied twice.
void main() {
    float gradient = smoothstep(0.0, 0.82, fragment_height);
    vec3 horizon = frame.fog_color_fog_end.rgb;
    vec3 zenith = max(frame.ambient_color_fog_start.rgb * 1.55, horizon * 0.42);
    vec3 color = mix(horizon, zenith, gradient);
    out_color = vec4(max(color, vec3(0.0)), 1.0);
}
