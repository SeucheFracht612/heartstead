# Terrain material authoring

Heartstead terrain uses one production shader and a data-driven material record per voxel type.
Full cubes, greedy quads, partial boxes, cross-plane cutouts, transparent fluids, and emissive
voxels all resolve through the same material table. Surface conditions are layers in that shader,
not separately compiled material combinations.

## Source files and runtime arrays

A voxel points to a material token:

```toml
kind = "voxel"
id = "base:voxels/grass"
terrain_material = "grass"
```

The voxel namespace and token resolve to `base:materials/grass`. Terrain material prototypes live
in `mods/<mod>/data/materials`:

```toml
kind = "material"
id = "base:materials/grass"
display_name = "Grass Material"
domain = "terrain"
blend_mode = "opaque"
shader_template = "base:shaders/templates/terrain.slang"

texture.albedo = "base:textures/voxels/grass.png"
texture.top = "base:textures/voxels/grass_top.png"
texture.side = "base:textures/voxels/grass_side.png"
scalar.roughness = "0.9"
scalar.normal_scale = "1.0"
scalar.texel_density = "1.0"
color.tint = "1.0,1.0,1.0,1.0"
```

The cooker decodes PNG or JPEG sources to bounded RGBA8. Runtime construction resizes terrain
layers to 128 × 128, generates a complete mip chain, and places them in three index-aligned arrays:

| Array | Color space | Channels |
| --- | --- | --- |
| Base color | sRGB | RGB color, A opacity |
| Normal | Linear | tangent X, tangent Y, tangent Z, unused A |
| Surface | Linear | R AO, G roughness multiplier, B metallic multiplier, A height/transition |

Missing normal maps are deterministically derived from base-color luminance. Missing surface maps
use AO=1, roughness multiplier=1, metallic multiplier=1, and neutral height. Authored maps are
preferred; derived normals are a robust fallback, not a replacement for source-quality tangent
normals.

Terrain uses linear minification/magnification, trilinear mip selection, repeat addressing, and
requests 8× anisotropic filtering. Vulkan enables sampler anisotropy when the selected physical
device supports it and clamps the request to the device limit.

## Face bindings and variants

The base binding names are:

- `albedo`: fallback for every face
- `side`: fallback for west, east, north, and south
- `west`, `east`, `north`, `south`
- `top`
- `bottom`

Resolution order is specific face, then `side` for lateral faces, then `albedo`. Normal and surface
maps add a role prefix:

```toml
texture.normal.albedo = "base:textures/voxels/stone_normal.png"
texture.normal.top = "base:textures/voxels/grass_top_normal.png"
texture.surface.albedo = "base:textures/voxels/stone_armh.png"
texture.surface.top = "base:textures/voxels/grass_top_armh.png"
```

Deterministic variants are contiguous and start at one:

```toml
texture.albedo = "base:textures/voxels/stone_0.png"
texture.albedo.variant.1 = "base:textures/voxels/stone_1.png"
texture.albedo.variant.2 = "base:textures/voxels/stone_2.png"
```

An auxiliary normal or surface set may contain one texture shared by every base variant, or the
same number of variants as the base set. Gaps and auxiliary count mismatches fail loading.

Variant choice hashes integer world cell coordinates, the face, and a fixed salt. The hash does
not include load order, chunk revision, neighboring block state, camera position, or floating
origin. The same hash optionally selects quarter rotations and mirroring:

```toml
scalar.stable_rotations = "1.0"
scalar.stable_mirroring = "1.0"
```

Only enable mirroring when the artwork has no directional meaning, text, handed tool marks, or
other features that would become invalid.

## Authored and sloped block geometry

Box-based partial blocks continue to use the `boxes` field in a block-model prototype. Slopes and
other authored terrain surfaces can use a semicolon-separated triangle list; each triangle is
three comma-separated XYZ positions in block-local space:

```toml
kind = "block_model"
id = "base:block_models/stone_ramp"
model_type = "custom_voxel"
triangles = "0,1,0,0,0,1,1,0,1;0,1,0,1,0,1,1,1,0"
render_bounds = "0,0,0,1,1,1"
neighbor_dependency_radius = "1"
mesh_invalidation_radius = "1"
```

Vertices use counter-clockwise winding as seen from outside. Degenerate, non-finite, out-of-bounds,
and under-declared-halo geometry fails prototype loading. Axis-aligned triangles on a cell boundary
participate in neighbor face culling, neighbor lighting, and corner AO. Non-axis-aligned faces keep
their authored geometric normal and use dominant-axis world projection, so a ramp shares the same
texel density and material layers as adjacent full or partial voxels. Authored faces are copied into
the immutable render-table snapshot before background meshing.

## PBR, macro, biome, and transition parameters

Supported scalar parameters and normal ranges are:

| Parameter | Meaning |
| --- | --- |
| `roughness` | Base perceptual roughness, 0..1 |
| `metallic` | Base metallic response, 0..1 |
| `ambient_occlusion` | Material AO multiplier, 0..1 |
| `emissive_strength` | Linear emissive intensity, nonnegative |
| `normal_scale` | Tangent-normal X/Y scale, nonnegative |
| `texel_density` | Texture repeats per voxel unit, positive |
| `biome_tint_strength` | Blend to `color.biome_tint`, 0..1 |
| `macro_color_strength` | Stable low-frequency color variation, 0..1 |
| `macro_roughness_strength` | Stable low-frequency roughness variation, 0..1 |
| `transition_width` | Softness/noise amplitude around the surface-map height transition, 0..0.5 |
| `transition_contrast` | Height-transition mask exponent, positive |
| `transition_noise_scale` | World-space transition noise frequency, positive |
| `unlit` | Values at least 0.5 bypass lighting |

`color.tint` multiplies the base texture. `color.biome_tint` is the material's response color for
the current biome channel. The present renderer supplies a deterministic macro field and
material-authored biome response; a future world-biome field can replace that scalar input without
changing material files or the GPU table.

The surface map's alpha channel is the authored height/transition signal. `transition_width`,
`transition_contrast`, and stable world noise shape that signal without introducing a repeated
dark line at each voxel or texture-tile edge.

World mapping reconstructs a periodic integer world position from chunk-local geometry plus a
packed chunk-coordinate key before applying texel density. Consequently arbitrary positive texel
densities remain continuous across chunk edges. Partial voxels use their real world dimensions, not
a stretched 0..1 box UV. The mapping never uses camera-relative position as an absolute coordinate,
so a floating-origin shift cannot cause texture swimming.

## Surface states

Solid `VoxelCell::state_bits` reserve bits 0..8 for:

| Bit | State |
| --- | --- |
| 0 | wetness |
| 1 | snow |
| 2 | frost |
| 3 | mud |
| 4 | moss |
| 5 | soot |
| 6 | heat |
| 7 | corruption |
| 8 | magical residue |

Bits 9..11 store shared coverage from 0..7. Bits 12..15 are reserved and rejected by the typed
decoder. Fluid cells keep their existing fluid-state encoding and are explicitly excluded from
solid surface decoding.

Each active state evaluates stable world noise, material susceptibility, shared coverage, and an
orientation mask where appropriate. All nine states run through the same fixed loop and can be
combined without pipeline or shader variants. Defaults are production-safe; a material can
override them:

```toml
scalar.surface.wetness.strength = "1.0"
scalar.surface.wetness.roughness = "0.12"
scalar.surface.snow.strength = "0.85"
scalar.surface.heat.emissive_strength = "2.5"
color.surface.moss.tint = "0.20,0.38,0.12,0.68"
```

An optional color/alpha overlay texture uses:

```toml
texture.overlay.moss = "base:textures/voxels/overlays/moss.png"
```

Overlay textures occupy aligned terrain-array layers and retain the same world-stable mapping.

## Geometry, AO, edits, and preview

The reference and greedy meshers calculate classic corner AO from the two side neighbors and
diagonal neighbor outside each boundary face. The four AO values are part of the greedy merge key,
preventing a merge from erasing an AO discontinuity. AO sampling reads the immutable chunk halo,
including unloaded/loaded neighbor identity and revision dependencies, so an edit across a chunk
boundary invalidates and rebuilds the affected mesh safely.

Chunk meshing remains asynchronous. Workers consume immutable voxel/render-table snapshots;
revision-checked results enter a typed mailbox; only the render owner thread uploads them. Rapid
edits coalesce, stale results are rejected, and visible near-player work retains priority.

Use the production-renderer preview scene for material and state review:

```sh
heartstead_render_benchmark --scene terrain-materials --vulkan \
  --frames 300 --warmup 60 --radius 1 --output terrain-materials.json
```

The scene contains repeated terrain bands and a raised 3 × 3 grid containing all nine surface
states. Use the existing base-color, normal, roughness, metallic, AO, emissive, shadow-cascade, and
light-grid debug views when diagnosing an authored material.
