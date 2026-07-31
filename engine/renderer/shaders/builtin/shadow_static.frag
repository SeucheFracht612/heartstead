#version 450

layout(location = 3) in vec2 fragment_uv0;
layout(location = 4) in vec2 fragment_uv1;
layout(location = 5) in vec4 fragment_color;
layout(location = 6) flat in uint fragment_layer;
layout(location = 7) flat in uint fragment_material;
layout(location = 9) in vec4 fragment_effect_parameters;
layout(location = 10) flat in uvec4 fragment_effect_metadata;

layout(set = 0, binding = 2) uniform sampler2DArray surface_textures;

struct GpuTextureBinding {
    uvec4 metadata;
    vec4 transform;
};

struct GpuSurfaceMaterial {
    GpuTextureBinding textures[5];
    vec4 base_color;
    vec4 emissive_metallic;
    vec4 roughness_normal_occlusion_alpha;
    uvec4 flags_and_padding;
};

layout(std430, set = 0, binding = 3) readonly buffer SurfaceMaterials {
    GpuSurfaceMaterial materials[];
} surface_materials;

const uint MATERIAL_ALPHA_TESTED = 1U;
const uint EFFECT_VEGETATION = 1U;

vec2 binding_uv(GpuTextureBinding binding) {
    vec2 uv = binding.metadata.z == 0U ? fragment_uv0 : fragment_uv1;
    uv *= binding.transform.zw;
    float rotation = uintBitsToFloat(binding.metadata.w);
    float sine = sin(rotation);
    float cosine = cos(rotation);
    return binding.transform.xy + mat2(cosine, sine, -sine, cosine) * uv;
}

void main() {
    // Consume the layer emitted by the shared skinned/morphed vertex path. Runtime texture layers
    // are densely allocated and never use the unsigned sentinel.
    if (fragment_layer == 0xffffffffU) {
        discard;
    }
    if ((fragment_effect_metadata.x & EFFECT_VEGETATION) != 0U &&
        fragment_effect_parameters.w < 1.0) {
        uint dither_hash =
            uint(gl_FragCoord.x) * 0x1f123bb5U ^
            uint(gl_FragCoord.y) * 0x5f356495U ^
            fragment_effect_metadata.y * 0x9e3779b9U;
        dither_hash ^= dither_hash >> 16U;
        if (float(dither_hash & 0xffffU) / 65535.0 >
            fragment_effect_parameters.w) {
            discard;
        }
    }
    GpuSurfaceMaterial material = surface_materials.materials[fragment_material];
    if ((material.flags_and_padding.x & MATERIAL_ALPHA_TESTED) == 0U) {
        return;
    }
    GpuTextureBinding binding = material.textures[0];
    float alpha =
        texture(surface_textures,
                vec3(binding_uv(binding), float(binding.metadata.x))).a *
        material.base_color.a * fragment_color.a;
    if (alpha < material.roughness_normal_occlusion_alpha.w) {
        discard;
    }
}
