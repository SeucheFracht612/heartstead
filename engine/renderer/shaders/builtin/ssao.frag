#version 450

layout(set = 0, binding = 0) uniform sampler2D scene_depth;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out float out_occlusion;
layout(location = 1) out vec2 out_depth_copy;

float random_angle(vec2 pixel) {
    return fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
}

void main() {
    float center = texture(scene_depth, in_uv).r;
    out_depth_copy = vec2(center, 0.0);
    if (center >= 0.99999) {
        out_occlusion = 1.0;
        return;
    }
    vec2 texel = 1.0 / vec2(textureSize(scene_depth, 0));
    float angle = random_angle(gl_FragCoord.xy);
    mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));
    float occlusion = 0.0;
    for (int sample_index = 0; sample_index < 8; ++sample_index) {
        float sample_angle = float(sample_index) * 0.78539816;
        vec2 direction = rotation * vec2(cos(sample_angle), sin(sample_angle));
        float sample_depth =
            texture(scene_depth, in_uv + direction * texel * 3.0).r;
        float delta = center - sample_depth;
        occlusion += smoothstep(0.0004, 0.008, delta);
    }
    out_occlusion = clamp(1.0 - occlusion * 0.085, 0.35, 1.0);
}
