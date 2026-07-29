# Asset Pipeline V1: Artist and Audio Guide

This guide explains what Heartstead accepts today, what each format is for, where finished files
go, and how to get them into the Foundation game. It is for contributors who create models,
textures, animation, particles, or audio and do not intend to change C++.

The dependable path in the current game is:

```text
finished source file
    -> asset catalog and dependency discovery
    -> production cooker
    -> versioned cooked manifest/store
    -> entity visual, voxel definition, or sound event
    -> renderer or audio system
```

## Quick format guide

| You are making | Deliver | Current use |
| --- | --- | --- |
| Static or skinned model | glTF 2.0 `.glb` or `.gltf` | Fully cooked and rendered through an `entity_visual` |
| Model base-color texture | 8-bit `.png`; `.jpg`/`.jpeg` for opaque art | Decoded to RGBA8, cooked with the model, and sampled by the model shader |
| Standalone texture | `.png`, `.jpg`, `.jpeg` | Decoded to a versioned RGBA8 payload, but not yet bindable to terrain, UI, or particles through contributor data |
| Prebuilt texture container | `.ktx2` | Container validation/cooking only; not a model-texture or runtime-transcoding path yet |
| Short sound effect | PCM/float `.wav` | Playable through a `sound_event`; mono 48 kHz is recommended for positional sound |
| Long ambience or music | `.flac` or PCM `.wav` | Playable through a `sound_event`; set `streaming = true` |
| Generated development sound | `.tone` | Playable, deterministic mono test/prototype audio |
| Ogg Vorbis audio | `.ogg` | Catalogued and production-cooked, but not playable in the current miniaudio build |
| Particle effect | `particle` prototype TOML | Procedural colored quads; no authored particle texture path yet |
| Font | `.ttf`, `.otf`, `.ttc` | Production-cookable SFNT data, but the game UI still uses its built-in diagnostic font |
| UI artwork | Keep source, normally export PNG for handoff | No authored UI atlas/skin import path yet |

For a new model, prefer a self-contained GLB. For recorded audio, prefer WAV for short effects and
FLAC for long files. PNG is the safest color-texture delivery.

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
description = "Replacement Foundation art."
target_namespace = "base"
```

This replaces `base:models/entities/storybook_player.gltf`. A resource pack cannot change gameplay
prototypes or redirect an existing ID to another extension. Use a mod when adding new content or
data definitions.

## Models, materials, textures, and animation

### Recommended Blender export

Use Blender's glTF 2.0 exporter and choose **glTF Binary (.glb)** when possible.

- Author in game units; the importer performs no project-specific unit conversion.
- Export meshes and animations, not Blender cameras or lights.
- Triangulate the model.
- Apply or clean transforms that cannot be represented as translation, quaternion rotation, and
  non-zero scale.
- Export normals and UV set 0.
- Give every material and animation clip a useful name.
- Confirm scale and forward direction in the asset brief. The pipeline does not impose an
  automatic facing conversion.

Text `.gltf` also works. Data URIs, embedded GLB data, and plain relative external buffer/image
paths are discovered. External paths may not be absolute and may not contain a query or fragment.
All dependencies must remain inside the asset catalog. The cooker includes the dependency closure
automatically.

GLB avoids missing sidecars and is the safest handoff. If using text glTF, keep `.bin` buffers and
PNG/JPEG images with the delivery and do not move them after export.

### Geometry supported by Model V1

The importer keeps:

- indexed triangle primitives;
- `POSITION`;
- `NORMAL` (missing normals become straight up and will light incorrectly);
- `TEXCOORD_0`;
- one node hierarchy with translation/rotation/scale transforms;
- static primitives;
- skinned primitives;
- `JOINTS_0` and `WEIGHTS_0`, with at most four influences per vertex;
- inverse-bind matrices;
- translation, rotation, and scale animation.

It does not currently use tangents, vertex colors, additional UV sets, morph targets, cameras, or
lights. `JOINTS_1`/`WEIGHTS_1` are rejected.

### Material and base-color rules

Material slots are preserved per glTF primitive. Model V1 supports:

- glTF `baseColorTexture`;
- glTF `baseColorFactor`;
- one PNG or JPEG base-color image per referenced texture;
- opaque materials;
- alpha-mask materials and `alphaCutoff`;
- `doubleSided`;
- a material with no base-color image, which uses the named white fallback texture.

The shader multiplies the sampled texture by the base-color factor. Base-color RGB is interpreted
as sRGB; alpha and the numeric material factor remain linear. Only `TEXCOORD_0` without a texture
transform is supported.

Use alpha mask for cutout leaves, grass cards, fences, and similar hard-edged transparency. glTF
alpha blend is rejected by the cooker. Normal, metallic/roughness, occlusion, and emissive maps are
outside V1 and are currently ignored rather than becoming final PBR materials.

PNG is recommended when alpha matters. JPEG is opaque and lossy. KTX2 cannot be used as a glTF
base-color image in V1.

Model images are decoded to bounded RGBA8 during cooking and stored in the versioned
`heartstead.model.v2` payload. An individual image may be at most 8192 pixels on either axis, and
the decoded images in one model share a 128 MiB limit.

### Static visual definition

An `entity_visual` connects an entity prototype to its cooked model:

```toml
kind = "entity_visual"
id = "base:visuals/oak_stump"
display_name = "Oak Stump Visual"
entity = "base:entities/oak_stump"
model = "base:models/props/oak_stump.glb"
bounds_padding = "0.15"
cast_shadow = "true"
```

Static models must not declare animation mappings.

The corresponding gameplay `entity` prototype is separate content data. Declaring a visual does
not spawn an entity by itself; a scenario or gameplay system must create and replicate that
entity. Once it is in the replicated scene, the shared model presentation system displays it
without an application or renderer change.

### Skinned player visual and named clips

Skinned Foundation player visuals map semantic roles to authored clip names:

```toml
kind = "entity_visual"
id = "base:visuals/player"
display_name = "Player Visual"
entity = "base:entities/player"
model = "base:models/entities/storybook_player.gltf"
bounds_padding = "0.20"
transition_ticks = "9"
cast_shadow = "true"

animations.idle = "idle"
animations.walk = "walk"
animations.run = "run"
animations.jump = "jump"
animations.fall = "fall"
animations.swim = "swim"

sounds.footstep_default = "base:audio/earth_footstep"
```

Clip names, not numeric positions, are used. Every mapped name must exist exactly once. For the
Foundation player, all six locomotion roles are required. The clips may use STEP, LINEAR, or
CUBICSPLINE interpolation and may animate node translation, rotation, or scale. Morph-weight
animation is rejected.

For other skinned entity types, the current general presenter still expects the same locomotion
role set. More specialized animation state maps are future work.

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

Real-time assets should remain far below these ceilings.

## Standalone textures

The production cooker recognizes `.png`, `.jpg`, `.jpeg`, and `.ktx2`.

- PNG/JPEG are the usable input formats for model base color.
- Standalone PNG/JPEG assets are decoded and production-cooked as bounded, versioned RGBA8
  payloads. Their color space remains unspecified until a material consumer binds them.
- KTX2 receives bounded container validation, but there is no runtime Basis/KTX transcode path.

The current terrain palette, UI atlas, and particle materials are renderer-generated. A standalone
texture prototype does not yet make an image appear on a voxel, UI widget, or particle. Do not
promise final terrain/UI/particle art through this path until those consumers are connected.

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

Ogg Vorbis passes catalog and production-container checks, but the current miniaudio build has no
Vorbis decoder. Do not deliver OGG as a playable asset yet. MP3, AAC/M4A, Opus, WMA, AIFF, and
MIDI are not part of the Heartstead pipeline.

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

Foundation particles are data-driven but currently untextured. A particle prototype controls
color, size, lifetime, motion, and atlas timing:

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

The atlas fields animate UV frames, but no contributor-authored particle atlas is bound yet.
Treat `material_group` as a project-assigned renderer group, not a free image reference.

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
terrain textures are not connected yet, so the block's `terrain_material` token and generated
renderer palette provide the visible Foundation material.

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
animation-role syntax, sound events, voxel feedback, and dependency discovery. A ready Foundation
content report has zero warnings and zero errors.

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

### 3. Cook the complete Foundation presentation set

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker \
  . build/foundation-presentation/asset_manifest.txt production \
  --presentation-assets --inspect
```

This reads all `entity_visual` and `sound_event` definitions, selects their active model and audio
assets, follows transitive glTF dependencies, converts each supported asset to its runtime format,
writes the cooked store, and reloads the store to verify it.

Building `heartstead_dev_game` runs this presentation cook automatically. Runtime audio consumes
these cooked payloads by logical ID; it does not need the source media after startup. New entity
visual models and sound-event assets do not require editing the application or a CMake asset list.

Use `--entity-visuals` when you intentionally want only the declared model set.

### 4. Preview in the Foundation game

```bash
cmake --build --preset default-debug -j2 --target heartstead_dev_game
./build/default-debug/apps/dev_game/heartstead_dev_game --no-save
```

Use the existing deterministic Foundation scene for model/material/animation and positional-audio
review. A model visual only appears when its entity is present in the replicated scene.

For a short automated startup check:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game --frames 3 --no-save
```

The bounded check proves startup/resource creation/shutdown, not artistic correctness.

## Fallbacks and failure behavior

Current named renderer resources include an error checker texture, white texture, black texture,
flat normal texture, error material, and error mesh.

- A glTF material without a base-color texture uses white.
- A model primitive without an imported material uses the renderer error material.
- An absent voxel surface sound uses the player visual's default footstep.
- Absent optional voxel break/place feedback uses the interaction fallback and logs the voxel ID,
  missing role, and selected fallback once per presentation-system lifetime.
- An entity with no registered visual uses `base:visuals/fallback`, whose visible primitive binds
  the named renderer error material. The unresolved entity prototype ID is logged once per
  presentation-system lifetime.
- Invalid prototype references fail content validation.
- An invalid image, model, or named clip fails production cooking/presentation initialization with
  the logical asset ID and reason.

A missing or uncookable model referenced by a declared visual remains a validation/startup error.
The unknown-entity fallback is a diagnostic safety net, not a substitute for delivering a valid
model.

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
- [ ] The model/audio is reviewed in the real Foundation executable.

## Common failures

- Uppercase letters or spaces in an asset path.
- A source/working file placed below `assets/`.
- A resource-pack replacement changed the extension and therefore the ID.
- A text glTF references a missing or escaping sidecar.
- A model is not triangulated.
- A model uses more than four joint influences or exports morph-weight animation.
- A base-color texture uses another UV set or a texture transform.
- A material uses alpha blend instead of opaque/mask.
- A mapped animation name is missing or duplicated.
- A valid sound file has no `sound_event`, or the event is never referenced by presentation data.
- OGG was assumed to be playable because it passed cooking.
- A standalone terrain/UI/particle texture was assumed to have a runtime consumer.

## Reference material

- [Khronos glTF 2.0 specification](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)
- [Blender glTF 2.0 exporter manual](https://docs.blender.org/manual/en/latest/addons/import_export/scene_gltf2.html)
- [KTX 2 specification](https://github.khronos.org/KTX-Specification/ktxspec.v2.html)
- [Heartstead asset architecture](architecture/assets.md)
- [Heartstead audio architecture](architecture/audio.md)
- [Heartstead animation architecture](architecture/animation.md)
