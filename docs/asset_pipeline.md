# Asset pipeline: artist and audio guide

This guide explains what Heartstead accepts today, what each format is for, where finished files
go, and how to get them into the development game. It is for contributors who create models,
textures, animation, particles, or audio and do not intend to change C++.

The dependable path in the current development runtime is:

```text
finished source file
    -> asset catalog and dependency discovery
    -> production cooker
    -> versioned cooked manifest/store
    -> visual, material, vegetation, voxel, particle, or sound definition
    -> renderer or audio system
```

## Quick format guide

| You are making | Deliver | Current use |
| --- | --- | --- |
| Static, rigid-node animated, or skinned model | glTF 2.0 `.glb` or `.gltf` | Fully cooked and rendered through a `visual_prefab` |
| Model material texture | 8-bit `.png`; `.jpg`/`.jpeg` for opaque art; Basis Universal `.ktx2` through `KHR_texture_basisu` | Cooked into the versioned model asset and sampled by the shared PBR shader |
| Standalone texture | `.png`, `.jpg`, `.jpeg`, `.ktx2` | Role-aware mip generation and BC5/BC7 or documented RGBA8 fallback |
| Texture cook settings | `<image>.texture.toml` | Color space, role, compression, mip, and alpha-coverage policy |
| Short sound effect | PCM/float `.wav` | Playable through a `sound_event`; mono 48 kHz is recommended for positional sound |
| Long ambience or music | `.flac` or PCM `.wav` | Playable through a `sound_event`; set `streaming = true` |
| Generated development sound | `.tone` | Playable, deterministic mono test/prototype audio |
| Ogg Vorbis audio | `.ogg` | Validated, production-cooked, and playable through a `sound_event` |
| Particle effect | `particle` prototype TOML | Data-driven billboard or mesh effect using configured material/mesh groups, flipbooks, lighting, blending, LOD, and bounded simulation |
| Font | `.ttf`, `.otf`, `.ttc` | Production-cookable SFNT data, but the game UI still uses its built-in diagnostic font |
| UI artwork | Keep source, normally export PNG for handoff | No authored UI atlas/skin import path yet |

For a new model, prefer a self-contained GLB. For recorded audio, prefer WAV for short effects and
FLAC for long files. PNG is the safest color-texture delivery.

Terrain has a separate data-driven face/variant/PBR and surface-state contract. See
[Terrain material authoring](terrain_material_authoring.md) for channel packing, face names,
deterministic variants, overlays, and the production preview scene.

Do not deliver FBX, OBJ, native Blender files, PSD/Krita files, MP3, or arbitrary working files as
runtime assets.

## Finished files and asset IDs

The base game is a mod. Finished runtime exports live below:

```text
mods/base/assets/
├── models/
│   ├── entities/
│   ├── items/
│   └── props/
├── textures/
│   ├── entities/
│   ├── items/
│   └── voxels/
├── sounds/
│   ├── ambient/
│   ├── effects/
│   └── footsteps/
├── music/
└── fonts/
```

A mod-owned file gets an ID from its mod namespace and its path below `assets/`:

```text
disk:  mods/base/assets/models/props/oak_stump.glb
ID:    base:models/props/oak_stump.glb

disk:  mods/base/assets/sounds/footsteps/stone_step.wav
ID:    base:sounds/footsteps/stone_step.wav
```

The extension is part of the ID. Renaming `stone_step.wav` to `stone_step.flac` changes the ID and
requires every reference to change.

Use only lowercase letters, digits, `_`, `-`, `/`, and `.`. Do not use spaces, uppercase letters,
backslashes, `..`, or symbolic links. A normal source asset is limited to 256 MiB.

Keep `.blend`, `.kra`, `.psd`, DAW projects, raw recordings, reference images, licenses, and export
notes outside any runtime `assets/` tree. The catalog treats every regular file under `assets/` as
a delivery.

## Replacing existing art with a resource pack

A resource pack can replace presentation files without changing gameplay prototypes. It must
provide the same logical path and extension under its `assets/` directory.

```text
resource_packs/my_art_pass/
├── resource_pack.toml
└── assets/
    └── models/
        └── entities/
            └── storybook_player.gltf
```

```toml
id = "my_art_pass"
name = "My Art Pass"
version = "1.0.0"
description = "Replacement base-game art."
target_namespace = "base"
```

This replaces `base:models/entities/storybook_player.gltf`. A resource pack cannot change gameplay
prototypes or redirect an existing ID to another extension. Use a mod when adding new content or
data definitions.

## Models, materials, textures, and animation

### Recommended Blender export

Use Blender's glTF 2.0 exporter and choose **glTF Binary (.glb)** when possible.

- Author in game units; the importer performs no project-specific unit conversion.
- Export cameras or punctual lights only when the asset deliberately uses them as retained
  metadata. Gameplay behavior is not inferred from Blender scene objects.
- Triangulate the model.
- Apply or clean transforms that cannot be represented as translation, quaternion rotation, and
  non-zero scale.
- Export normals, tangents, and the UV sets used by materials.
- Give every material and animation clip a useful name.
- Confirm scale and forward direction in the asset brief. The pipeline does not impose an
  automatic facing conversion.

Text `.gltf` also works. Data URIs, embedded GLB data, and plain relative external buffer/image
paths are discovered. External paths may not be absolute and may not contain a query or fragment.
All dependencies must remain inside the asset catalog. The cooker includes the dependency closure
automatically.

GLB avoids missing sidecars and is the safest handoff. If using text glTF, keep `.bin` buffers and
PNG/JPEG images with the delivery and do not move them after export.

### Geometry supported by Model v5

The importer keeps:

- triangle-list primitives (the cooker generates indices when the source omits them);
- `POSITION`;
- `NORMAL` (missing normals become straight up and will light incorrectly);
- authored or cooker-generated `TANGENT`;
- `TEXCOORD_0` and `TEXCOORD_1`;
- normalized or floating-point `COLOR_0`;
- one node hierarchy with translation/rotation/scale transforms;
- static primitives;
- unskinned primitives attached to animated nodes;
- skinned primitives;
- `JOINTS_0`/`WEIGHTS_0` and optional `JOINTS_1`/`WEIGHTS_1`; the strongest four influences are
  retained and normalized for the GPU;
- inverse-bind matrices;
- position, normal, and tangent morph targets;
- translation, rotation, scale, and morph-weight animation;
- quantized attributes through `KHR_mesh_quantization`;
- `EXT_meshopt_compression` and `KHR_draco_mesh_compression`, decoded once during cooking.
- named sockets (`socket_`, `socket:`, or `anchor_` node naming);
- node-authored LOD groups, collision metadata, cameras, and `KHR_lights_punctual`.

Only triangle-list primitives and UV sets 0 and 1 are accepted.

### Material rules

Material slots are preserved per glTF primitive. Model v5 supports core glTF
metallic-roughness materials:

- glTF `baseColorTexture`;
- glTF `baseColorFactor`;
- `metallicRoughnessTexture`, metallic factor, and roughness factor;
- normal maps and normal scale;
- occlusion maps and strength;
- emissive maps and emissive factor;
- PNG, JPEG, or Basis Universal KTX2 images;
- each texture's sampler wrap/minification/magnification state;
- `TEXCOORD_0`, `TEXCOORD_1`, and `KHR_texture_transform`;
- opaque materials;
- alpha-mask materials and `alphaCutoff`;
- alpha-blended materials;
- `doubleSided`;
- required or optional `KHR_materials_unlit`;
- named white, black, normal, and error fallback layers where a map is absent.

Base-color and emissive RGB are sampled as sRGB. Metallic-roughness, normal, occlusion, alpha, and
numeric material factors remain linear. Color and data maps are therefore stored in separate GPU
arrays, and an image is uploaded to both only when materials use it in both roles.

Use alpha mask for cutout leaves, grass cards, fences, and similar hard-edged transparency. glTF
alpha blend uses the transparent render layer and back-to-front object sorting. PNG is recommended
when alpha matters, JPEG is opaque and lossy, and Basis Universal KTX2 is useful for compact model
delivery.

Model images are decoded to bounded RGBA8 during cooking and stored in the versioned
`heartstead.model.v5` payload. An individual image may be at most 8192 pixels on either axis, and
the decoded images in one model share a 128 MiB limit.

`KHR_materials_unlit` is supported for both optional and required glTF extensions. Unlit materials
retain their base-color texture/factor, alpha mode, and double-sided state, but bypass sun and
ambient lighting. They still participate in scene fog. This is stored as a material flag and uses
the shared static-mesh shader, so it does not create extra textures, draw calls, or pipeline
variants. The base Kenney character is authored this way, so its texture should retain the same
flat-shaded color relationship as an unlit Blender/glTF preview; Heartstead does not add specular
lighting that the material did not request.

### Static visual prefab

A `visual_prefab` connects gameplay-owned entity state to cooked presentation data. The legacy
`entity_visual` kind remains readable for migration, but new content should use `visual_prefab`:

```toml
kind = "visual_prefab"
id = "base:visuals/oak_stump"
display_name = "Oak Stump Visual"
entity = "base:entities/oak_stump"
model = "base:models/props/oak_stump.glb"
model_scale = "1.0"
bounds_padding = "0.15"
cast_shadow = "true"
preview.lighting = "studio"
preview.camera_distance = "4.0"
```

A static visual has no animation mappings. This describes presentation behavior, not mesh layout:
an unskinned model may still contain animation clips and becomes animated when its visual maps
locomotion roles to those clips.

`model_scale` is an optional positive uniform presentation scale and defaults to `1.0`. Use it when
an otherwise correctly authored model uses different world-unit proportions from the entity's
gameplay representation. It scales the complete model pose around the model origin; it does not
change the controller, collider, authoritative transform, or replicated state.

The corresponding gameplay `entity` prototype is separate content data. Declaring a visual does
not spawn an entity by itself; a scenario or gameplay system must create and replicate that
entity. Once it is in the replicated scene, the shared model presentation system displays it
without an application or renderer change.

### Animated player visual and named clips

Animated player visuals map semantic roles to authored clip names. The model may use rigid
mesh-bearing nodes, skinning, morph targets, or a combination:

```toml
kind = "visual_prefab"
id = "base:visuals/player"
display_name = "Player Visual"
entity = "base:entities/player"
model = "base:models/entities/test_player.glb"
model_scale = "0.66"
bounds_padding = "0.40"
transition_ticks = "9"
cast_shadow = "true"

animations.idle = "idle"
animations.walk = "walk"
animations.run = "sprint"
animations.jump = "idle"
animations.fall = "idle"
animations.swim = "idle"

sounds.footstep_default = "base:audio/earth_footstep"
```

Clip names, not numeric positions, are used. Every mapped name must exist exactly once. For the
base player visual, all six locomotion roles are required. The clips may use STEP, LINEAR, or
CUBICSPLINE interpolation and may animate node translation, rotation, scale, or morph weights.

Skin presence is not an animation requirement. A visual with mappings is rejected only when its
model has no animation clips. At runtime both rigid-node and skinned models sample a shared local
node pose, blend transitions over `transition_ticks`, and evaluate the node hierarchy. Rigid
primitives use their owning node matrix with the ordinary mesh shader; skinned primitives construct
joint palettes from the same evaluated matrices.

Locomotion is in-place: the player controller owns world movement, while presentation removes
horizontal root motion from common root-motion carrier nodes. `bounds_padding` expands
conservative per-primitive rigid bounds, including morph-target displacement. Increase it if an
asset's rotations or unusual deformation still leave the maintained bounds. Padding is expressed
in model-local units and is scaled by `model_scale` together with the resulting bounds.

State mappings use `states.<channel>.<value>.*`; gameplay replicates channel/value state and the
prefab maps it to a model, named animation, material parameters, and visibility groups. External
LOD models use `lods.<N>.model` plus `lods.<N>.max_distance`, with `0` on the final unbounded LOD.
Named equipment/effect/light anchors reference cooked sockets. See
[Asset conventions](asset_conventions.md) for the complete schema.

### Model safety ceilings

These are malformed-input limits, not art budgets:

| Data in one model | Limit |
| --- | ---: |
| Source and declared buffers | 256 MiB |
| Vertices | 1,000,000 |
| Indices | 3,000,000 |
| Nodes | 16,384 |
| Primitives | 16,384 |
| Images | 1,024 |
| Materials | 4,096 |
| Skins | 256 |
| Joints in one skin | 256 |
| Animation clips | 256 |
| Channels in one clip | 4,096 |
| Keys in one channel | 1,000,000 |
| Morph targets on one primitive | 64 |
| Morph delta values | 16,777,216 |

Real-time assets should remain far below these ceilings.

## Standalone textures

The production cooker recognizes `.png`, `.jpg`, `.jpeg`, and `.ktx2`.

- PNG/JPEG and supported KTX2 sources are decoded offline.
- Role inference and optional `.texture.toml` settings select sRGB/linear handling and BC5, BC7,
  or RGBA8 output.
- The cooker generates the full mip chain. Normal mips are renormalized and mask mips can preserve
  alpha-test coverage.
- BC5/BC7 payloads stay GPU-native at runtime. The terrain/voxel shared-array path is the explicit
  RGBA8 fallback because that array currently requires CPU-resizable pixels.

Terrain materials can bind standalone production-cooked PNG/JPEG assets. The renderer resamples
each source image into its 16 x 16 sRGB terrain texture array and generates mipmaps. The UI atlas
remains built in. Particle prototypes select project-configured material groups rather than binding
an arbitrary standalone logical texture ID directly.

The checked-in `.txt` files below some texture/UI folders are development placeholders, not image
formats to copy.

## Audio

### Recommended export settings

| Use | File | Channels | Suggested event settings |
| --- | --- | --- | --- |
| Footstep, impact, placement, tool SFX | 48 kHz PCM WAV, 16- or 24-bit | Mono | spatialized, not streaming |
| UI SFX | 48 kHz PCM WAV | Mono or stereo | non-spatial, not streaming |
| Long ambience | FLAC or PCM WAV | Mono if positioned, stereo if global | usually streaming |
| Music | FLAC or PCM WAV | Stereo | music bus, non-spatial, streaming |

WAV may contain uncompressed PCM or IEEE floating-point samples. FLAC must be a native FLAC
stream. The engine can resample, but 48 kHz matches the default output and avoids an unnecessary
conversion.

Looping is whole-file looping; authored loop-point metadata is not consumed. Make the end join the
beginning cleanly.

Ogg files must contain Vorbis audio. They pass production-container validation and are decoded by
the miniaudio runtime through stb_vorbis. Opus-in-Ogg is not accepted. MP3, AAC/M4A, WMA, AIFF,
and MIDI are not part of the Heartstead pipeline.

### Sound event

An audio file is used through a `sound_event` prototype:

```toml
kind = "sound_event"
id = "base:audio/stone_footstep"
display_name = "Stone Footstep"
asset = "base:sounds/footsteps/stone_step.wav"
bus = "sfx"
gain = 0.85
minimum_distance = 1.0
maximum_distance = 24.0
priority = 160
maximum_instances = 12
spatialized = true
looping = false
streaming = false
```

Put it under `mods/<mod-id>/data/audio/`. `asset` must resolve to an active sound/music asset.
Content validation rejects missing assets and wrong prototype kinds before a session starts.

Available buses are `master`, `music`, `sfx`, and `ambient`. `minimum_distance` begins attenuation;
`maximum_distance` is the far limit. Priority is 0–255. `maximum_instances` is the simultaneous
voice budget for that event.

### Connecting block sounds and particles

A voxel selects accepted break/place feedback and its walking surface sound from content:

```toml
interaction.break_particle = "base:particles/stone_break_chip"
interaction.break_sound = "base:audio/stone_block_break"
interaction.place_sound = "base:audio/stone_block_place"
interaction.footstep_sound = "base:audio/stone_footstep"
```

Break and placement effects play only after the authoritative edit is accepted and replicated.
Rejected edits do not emit success feedback. A collidable voxel should declare a footstep sound.
If it does not, player footsteps use `sounds.footstep_default` from the player visual.

### Procedural `.tone`

`.tone` is a source-controlled development format for deterministic mono audio:

```toml
wave = "noise"
frequency_hz = 155
amplitude = 0.18
duration_ms = 90
attack_ms = 1
release_ms = 75
seed = 1398030681
```

`wave` is `sine` or `noise`. Frequency is 20–20,000 Hz, amplitude is 0–1, and duration is at most
60 seconds. Attack plus release may not exceed the duration. Use `.tone` for prototypes or
intentionally synthetic effects, not as the preferred final format for recorded sound.

## Particles

Particles are data-driven billboard or mesh effects. A prototype selects project-configured
material and mesh groups and controls color, size, lifetime, motion, alignment, shading, blending,
LOD behavior, optional collision, and flipbook timing:

```toml
kind = "particle"
id = "base:particles/stone_break_chip"
display_name = "Stone Break Chip"
material_group = "1"
lifetime_min_seconds = "0.35"
lifetime_max_seconds = "0.80"
speed_min = "0.7"
speed_max = "2.8"
direction_spread = "0.72"
gravity = "-5.5"
drag = "1.1"
size_min = "0.06"
size_max = "0.16"
end_size_multiplier = "0.8"
start_color = "0.48,0.50,0.54,0.95"
end_color = "0.24,0.25,0.28,0.0"
atlas_columns = "2"
atlas_rows = "2"
atlas_frame_count = "4"
atlas_frames_per_second = "12"
```

The atlas fields animate UV frames inside the selected material group. Treat `material_group`
as a project-assigned renderer group, not a free image reference; adding a new particle atlas still
requires registering and binding the corresponding project material group.

## Voxel/block visual data

Voxel shapes are content data, not arbitrary rotated planes. A slope in Heartstead is a terraced
arrangement of full and partial voxel shapes.

A voxel prototype may provide:

- `block_model`;
- logical occupancy (`full`, `partial`, `decorative`, or `fluid`);
- collision, selection, and occlusion bounds;
- light absorption/emission;
- terrain material token;
- break/place particle and sound;
- footstep surface sound.

Current cube and partial block models are generated from box definitions. Standalone authored
terrain textures are resolved through the block's `terrain_material` token. For example,
`terrain_material = "grass"` on `base:voxels/grass` selects `base:materials/grass`:

```toml
kind = "material"
id = "base:materials/grass"
domain = "terrain"
blend_mode = "opaque"
shader_template = "base:shaders/templates/terrain.slang"
texture.albedo = "base:textures/voxels/grass.png"
texture.top = "base:textures/voxels/grass_top.png"
texture.top.variant.1 = "base:textures/voxels/grass_top_variant_1.png"
texture.top.variant.2 = "base:textures/voxels/grass_top_variant_2.png"
texture.bottom = "base:textures/voxels/dirt.png"
texture.west = "base:textures/voxels/grass_west.png"
texture.east = "base:textures/voxels/grass_east.png"
texture.north = "base:textures/voxels/grass_north.png"
texture.south = "base:textures/voxels/grass_south.png"
scalar.roughness = "0.9"
color.tint = "1.0,1.0,1.0,1.0"
```

`texture.albedo` is the all-face fallback. `texture.side` may replace all four lateral faces, and
`texture.top`, `texture.bottom`, `texture.west`, `texture.east`, `texture.north`, and
`texture.south` may replace individual faces. Directional names map to `-X`, `+X`, `-Z`, and `+Z`
respectively. Resolution is most-specific face, then `side` for lateral faces, then `albedo`;
faces with no resolved texture retain the generated diagnostic palette.

Every primary binding may declare hand-authored alternatives with contiguous, one-based
`.variant.N` suffixes. For example, `texture.top.variant.1` accompanies `texture.top`. A variant
family must declare its own primary texture, and gaps such as variants 1 and 3 without 2 are
rejected. The files are complete authored tiles—not procedural pixel changes. At runtime the
renderer deterministically selects one tile for each block face from its authored set. The choice
is stable for a world coordinate, needs no saved or replicated state, and continues to work inside
greedy-merged quads.

Resource packs can replace any primary or variant at the same logical ID.

## Build, import, and validation

Run commands from the repository root.

### 1. Validate IDs and content references

```bash
cmake --preset default-debug
cmake --build --preset default-debug \
  --target heartstead_mod_validator heartstead_asset_cooker

./build/default-debug/tools/mod_validator/heartstead_mod_validator .
```

The validator checks mods, resource packs, prototypes, visual definitions, asset references,
animation-role syntax, sound events, voxel feedback, and dependency discovery. A ready base-content
report has zero warnings and zero errors.

### 2. Production-cook one delivery

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker \
  . build/asset-check/asset_manifest.txt production \
  --only base:models/props/oak_stump.glb \
  --inspect
```

Use the complete logical ID. `--only` automatically includes discovered dependencies.

For audio:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker \
  . build/asset-check/asset_manifest.txt production \
  --only base:sounds/footsteps/stone_step.wav \
  --inspect
```

The development cooker is permissive. A production cook is the authoritative media/import check.

### 3. Cook the complete base presentation set

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker \
  . build/base-presentation/asset_manifest.txt production \
  --presentation-assets --inspect
```

This reads `visual_prefab`/legacy `entity_visual`, vegetation-species, `sound_event`, and material
definitions. It selects declared visual models, vegetation LOD and growth-override models, audio
assets, and material texture references; follows transitive dependencies; converts supported
assets; writes the cooked store; and reloads it for verification.

Applications that consume the production presentation store, including `heartstead_dev_game`, the
renderer benchmark, and Asset Lab, depend on this cook. Runtime systems consume cooked payloads by
logical ID and do not need source media after startup. Adding a referenced presentation asset does
not require editing an application-owned CMake asset list.

Use `--entity-visuals` when you intentionally want only the declared model set.

### 4. Preview in Asset Lab

```bash
cmake --build --preset default-debug --target heartstead_asset_lab
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --prefab base:visuals/player --preview visual-prefab --lighting studio
```

Use `--headless` for deterministic import inspection and `--list` for every preview, lighting, and
debug name. Asset Lab uses the same cooked store, renderer, shaders, material system, model
presentation, visual-prefab state mapping, and particle presentation as gameplay.

### 5. Preview in the development game

```bash
cmake --build --preset default-debug -j2 --target heartstead_dev_game
./build/default-debug/apps/dev_game/heartstead_dev_game --no-save
```

Use the existing deterministic development scene for model/material/animation and positional-audio
review. A model visual only appears when its entity is present in the replicated scene.

For a short automated startup check:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game --frames 3 --no-save
```

The bounded check proves startup/resource creation/shutdown, not artistic correctness.

For a deliberate native fallback review:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game \
  --no-save --diagnostic-asset-fallbacks
```

This opt-in probe adds a missing-model visual, an animated visual with only its required idle
mapping, and an unregistered positional sound request. The overlay and log identify the missing
logical ID, source/cooked path, failing dependency, and selected fallback. It never changes the
authoritative development scene or save.

## Fallbacks and failure behavior

Current named renderer resources include an error checker texture, white texture, black texture,
flat normal texture, error material, and error mesh.

- A glTF material without a base-color texture uses white.
- A glTF primitive without a material uses the core glTF default white PBR material. A malformed
  runtime primitive whose material cannot be resolved uses the renderer error material.
- An absent voxel surface sound uses the player visual's default footstep.
- Absent optional voxel break/place feedback uses the interaction fallback and logs the voxel ID,
  missing role, and selected fallback once per presentation-system lifetime.
- A runtime request for an unregistered sound event uses
  `base:audio/interaction_fallback`. The request keeps its emitter, while the fallback supplies the
  cooked asset and playback policy. The audio diagnostics report the missing and fallback IDs once
  per distinct missing ID.
- An entity with no registered visual uses `base:visuals/fallback`, whose visible primitive binds
  the named renderer error material. The unresolved entity prototype ID is logged once per
  presentation-system lifetime.
- A declared runtime visual whose model record is absent from the cooked store uses the fallback
  visual's model and records its logical ID, source/cooked path, dependency, error, and fallback.
- A missing or invalid non-idle locomotion mapping uses the visual's required named `idle` mapping
  and emits the same structured diagnostic. The required idle mapping itself cannot fall back.
- Invalid prototype references fail content validation.

An invalid image or model source, a missing asset-catalog dependency, the fallback model itself, or
the required idle clip remains a validation/cook/startup error with the logical ID and reason.
Likewise, a declared sound event with a missing or uncookable asset is a content error rather than
a fallback case. Runtime fallbacks cover unavailable runtime records and requests; they are
diagnostic safety nets, not substitutes for delivering valid definitions and assets.

## Remaining glTF gaps

The v5 path closes the common Blender/exporter holes, but it is not every glTF extension:

- primitive modes other than triangle lists;
- UV sets above `TEXCOORD_1`;
- material extensions such as clearcoat, sheen, transmission/volume, IOR, specular, iridescence,
  anisotropy, emissive strength, and dispersion;
- WebP/AVIF texture extensions, material variants, GPU instancing, and animation pointers;
- ASTC/ETC runtime targets (desktop production uses BC5/BC7);
- per-triangle ordering for intersecting alpha-blended surfaces. Transparent objects are sorted
  back to front as objects.

A required unsupported extension fails during strict import. Optional unsupported extensions may
be ignored as glTF permits, so do not rely on them for the model's essential appearance.

## Delivery checklist

- [ ] Finished export uses a supported current format.
- [ ] Disk path and complete `namespace:path.ext` ID are recorded.
- [ ] Working files are outside the runtime `assets/` tree.
- [ ] License, author, and source/reference information accompany the handoff.
- [ ] Model scale, facing, triangle count, material count, and rig/joint count are recorded.
- [ ] Every animation clip has a unique stable name; required roles are mapped in the visual.
- [ ] Base-color texture color space and alpha-mask intent are recorded.
- [ ] Audio sample rate, channels, loop, spatial/global intent, bus, and suggested gain are
      recorded.
- [ ] `heartstead_mod_validator` reports zero errors.
- [ ] The focused production cook succeeds.
- [ ] The model/audio is reviewed in the real development executable.

## Common failures

- Uppercase letters or spaces in an asset path.
- A source/working file placed below `assets/`.
- A resource-pack replacement changed the extension and therefore the ID.
- A text glTF references a missing or escaping sidecar.
- A model is not triangulated.
- A model uses a non-triangle primitive mode or UV set 2 or higher.
- A material requires an unsupported PBR extension such as transmission or clearcoat.
- A KTX2 image is not Basis Universal (or plain RGBA8 for the low-level decoder).
- A mapped animation name is missing or duplicated.
- A valid sound file has no `sound_event`, or the event is never referenced by presentation data.
- An `.ogg` file contains Opus or another codec instead of Vorbis.
- A standalone UI texture or arbitrary per-particle texture ID was assumed to have a runtime
  consumer.
- A terrain material references KTX2 even though runtime terrain currently requires PNG/JPEG.

## Reference material

- [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
- [Blender glTF 2.0 exporter manual](https://docs.blender.org/manual/en/latest/addons/import_export/scene_gltf2.html)
- [KTX 2 specification](https://github.khronos.org/KTX-Specification/ktxspec.v2.html)
- [Heartstead asset architecture](architecture/assets.md)
- [Heartstead audio architecture](architecture/audio.md)
- [Heartstead animation architecture](architecture/animation.md)
