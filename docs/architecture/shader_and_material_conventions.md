# Shader and Material Conventions

Status: authoritative for Renderer V2.

World lighting is linear HDR. World shaders output linear radiance to `rgba16_sfloat` and never apply
gamma. Exposure, bloom combination, tone mapping, and linear-to-sRGB conversion happen once in
`tone_map.frag`; UI renders afterward and is exposure independent.

World coordinates passed to shaders are camera-relative floats derived from double-precision anchors.
Stable texture mapping and deterministic variation use absolute block/chunk identity, never rebased
local position.

The standard surface contract is base color, tangent-space normal, roughness, metallic, AO, emissive,
alpha mode, double-sided/unlit flags, vertex color, and two UV sets. Missing channels use neutral
fallbacks. Terrain and imported meshes share environment/direct lighting, shadows, and debug views.

Descriptor names are shader-interface ABI. Pipeline creation validates layout, vertex attributes,
attachment formats, blending, culling, depth state, and push constants. Add variants to prewarm before
sealing the cache. Opaque/masked surfaces write depth; cutout thresholds match shadow passes;
transparent surfaces use the dedicated ordered pass; SDF text stores coverage in a linear atlas.

See [Terrain authoring](../terrain_material_authoring.md) and [Asset conventions](../asset_conventions.md).
