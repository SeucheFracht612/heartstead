# Production asset conventions

Heartstead model, texture, animation, and audio runtime paths consume cooked assets, never
general-purpose glTF or source images. Source files are catalogued by stable
`namespace:path.ext` IDs and production cooking emits a schema-v2 manifest plus versioned
payloads. Built-in SPIR-V and the packaged UI font are explicit CMake-staged renderer bootstrap
assets; they do not establish a general source-asset runtime path.

## Models

- Deliver glTF 2.0 GLB where practical. Use metres/game units, triangle primitives, named
  materials, named clips, `TEXCOORD_0/1`, vertex colors, and authored tangents.
- Prefix socket nodes with `socket_`, `socket:`, or `anchor_`.
- Name collision-only nodes with the project collision prefix and LOD nodes with a stable LOD
  suffix. The cooker records both as metadata and excludes collision-only geometry from rendering.
- Keep alpha-test foliage in `MASK`, glass in `BLEND`, and use `doubleSided` only when required.
- The cooker accepts rigid-node animation, skinning, morph targets, meshopt/Draco source
  compression, cameras, and `KHR_lights_punctual`. Runtime geometry is `heartstead.model.v5`.

## Textures

Place an optional `<image>.texture.toml` beside a standalone image:

```toml
role = "normal"
color_space = "linear"
compression = "bc5"
generate_mips = true
preserve_alpha_coverage = false
alpha_cutoff = "0.5"
```

Roles are `color`, `normal`, `metallic_roughness`, `occlusion`, `emissive`, `mask`,
`user_interface`, and `data`. Production output is `heartstead.texture.v2`. Color/emissive/UI
textures default to sRGB BC7; normal maps default to linear BC5; other data defaults to linear
BC7. Mask textures can preserve alpha coverage. Terrain textures use the documented RGBA8 fallback
because the voxel renderer resizes them into a shared array.

## Visual prefabs

Use `kind = "visual_prefab"`. Required fields are `entity` and `model`. Common optional mappings:

```toml
shadow_policy = "cast"
bounds = "-0.5,0.0,-0.5,0.5,2.0,0.5"

lods.0.model = "base:models/buildings/forge.glb"
lods.0.max_distance = "48"
lods.1.model = "base:models/buildings/forge_lod1.glb"
lods.1.max_distance = "0"

socket_aliases.right_hand = "hand_r"
anchors.equipment.tool = "right_hand"
anchors.effect.smoke = "chimney"
anchors.light.fire = "hearth"

groups.door = "Door,DoorHandle"
states.open.true.groups.door = "false"
states.open.true.animation = "Open"
states.powered.true.priority = "20"
states.powered.true.materials.Embers = "base:materials/hot_embers"

preview.lighting = "fire_lit_interior"
preview.camera_distance = "5"
preview.state.powered = "true"
```

Gameplay owns and replicates `open`, `powered`, `fill_level`, `heat`, growth, damage, and similar
state. The visual prefab only maps those values to presentation. Rules are selected
deterministically by priority and then channel/value. Material slots, groups, sockets, model IDs,
and named clips are validated before use.

## Determinism and iteration

The catalog, dependency closure, model conversion, mesh optimization, texture compression, and
manifest encoding are deterministic. Re-cooking unchanged inputs reuses payloads. A source,
sidecar, or transitive dependency change invalidates affected outputs. Use Asset Lab headless mode
in CI and the native mode for visual sign-off.
