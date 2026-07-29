#version 450

layout(location = 0) in vec3 fragment_normal;
layout(location = 1) in vec3 fragment_world_position;
layout(location = 2) in vec2 fragment_uv;
layout(location = 3) in vec4 fragment_color;
layout(location = 4) flat in uint fragment_layer;

layout(push_constant) uniform FramePushConstants {
    mat4 view_projection;
    vec4 unused_origin;
    vec4 sun_direction_intensity;
    vec4 ambient_color_fog_start;
    vec4 fog_color_fog_end;
} frame;

layout(location = 0) out vec4 out_color;

const uint LAYER_ALPHA_TESTED = 1U;
const uint LAYER_TRANSPARENT = 2U;

vec3 linear_to_srgb(vec3 linear_color) {
    vec3 low = linear_color * 12.92;
    vec3 high = 1.055 * pow(max(linear_color, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(linear_color, vec3(0.0031308)));
}

void main() {
    if (fragment_layer == LAYER_ALPHA_TESTED && fragment_color.a < 0.5) {
        discard;
    }
    vec3 normal = normalize(fragment_normal);
    vec3 sun_direction = normalize(frame.sun_direction_intensity.xyz);
    float directional = frame.sun_direction_intensity.w *
                        max(dot(normal, sun_direction), 0.0);
    vec3 color = fragment_color.rgb *
                 (frame.ambient_color_fog_start.rgb + vec3(directional));
    float fog = smoothstep(frame.ambient_color_fog_start.w, frame.fog_color_fog_end.w,
                           length(fragment_world_position));
    color = mix(color, frame.fog_color_fog_end.rgb, fog);
    float alpha = fragment_layer == LAYER_TRANSPARENT ? fragment_color.a : 1.0;
    out_color = vec4(linear_to_srgb(max(color, vec3(0.0))), alpha);
}
