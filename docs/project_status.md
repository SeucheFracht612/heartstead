# Project status

This page records the maintained implementation status of Heartstead. It is not a roadmap or a
promise that every architecture target is complete.

**Audit baseline:** repository commit `7ee2cf940ecd3dfdffce48eef2bc0e3be4646260`
(`docs(rendering): document environmental systems`). Update the baseline when this page is
re-audited.

## What works now

### Development runtime

`heartstead_dev_game` composes an authoritative server and a client in one process for local play.
It can also run as a remote client against `heartstead_dedicated_server`. The development scene
exercises voxel editing, movement, camera modes, inventory UI, models, animation, audio, particles,
profile-driven environment state, lighting, fluids, saves, networking, and diagnostics. The
selected environment profile is combined with the deterministic solar path, and swimming selects
the underwater presentation profile.

This is a technical integration slice. It is not yet the intended settlement-survival game loop,
content progression, or polished player experience.

### World and simulation foundation

The current engine includes signed 64-bit block/chunk coordinates, 32-cubed chunks, palette-backed
voxels, dirty-region propagation, asynchronous chunk meshing, lighting and fluid foundations,
deterministic world generation, entities, rooms, spatial networks, processes, workpieces, build
pieces, assemblies, cargo, transactions, commands, snapshots, and replay support.

### Presentation and assets

The native renderer targets Vulkan 1.3 on Linux/X11 and uses dynamic rendering, camera-relative
coordinates, and a bounded dependency-validated frame graph. The maintained path renders into a
linear HDR target, applies SSAO, FXAA, bloom, tone mapping, and UI composition, and supports PBR
sun, point, and spot lighting. Directional shadows use four cascades; a bounded local budget provides
two spotlight shadow maps. The headless backend validates the same renderer-neutral contracts in
tests and tools.

The production environment stack includes data-driven atmosphere, clouds, fog, weather, voxel and
large-body water, vegetation, textured billboard and mesh particles, trails, and surface marks.
The development game consumes profile-driven environment state; the `starting-biome` renderer
benchmark composes the broader environment stack into a stable integration workload.

The asset pipeline discovers source assets through mods/resource packs, validates and cooks them,
and loads versioned runtime payloads. Model v5 retains the project's supported glTF geometry, PBR
materials, rigid-node and skinned animation, morph targets, sockets/anchors, LOD metadata, cameras,
and punctual lights. Standalone texture cooking performs role-aware mip generation and BC5/BC7 delivery with
documented RGBA8 fallbacks. Visual prefabs provide state selection, external LODs, fallback chains,
and socket/anchor mapping. `heartstead_asset_lab` inspects those production-cooked paths through
the same renderer and presentation systems used by gameplay. The exact contributor-facing format
boundary lives in [the asset pipeline guide](asset_pipeline.md).

### Multiplayer

The server owns commands and world truth. Local sessions use the in-memory transport. Remote
clients and the dedicated server use project-owned POSIX UDP with numeric IPv4 endpoints,
challenge-cookie admission, session tokens, bounded fragmentation/reassembly, ACK/retry/drop,
keepalives, timeouts, rate limits, reliable world events, unreliable latest-wins movement, client
prediction/reconciliation, and remote interpolation.

The remote transport is intended for controlled LAN and testing. It does not provide encryption,
account authentication, forward secrecy, NAT traversal, matchmaking, or a complete congestion
controller.

### Persistence

The local development game can create and reopen a versioned save database, including world data,
profiles, discovery, logs, migrations, and stable prototype mappings. Missing content is retained
through explicit placeholder behavior rather than silently discarded.

The standalone dedicated-server executable currently creates an in-memory authoritative world. It
has no `--save` option and does not persist its session across restarts. Treat dedicated-server
persistence as not implemented until the executable owns the save lifecycle explicitly.

### Modding and scripting

Base content is loaded from `mods/base` through the public prototype/mod pipeline. The engine
supports ordered lifecycle stages, patch provenance, semantic validation, resource-pack overrides,
compatibility fingerprints, and inspection tools.

The production script backend embeds Luau behind an engine-owned sandbox. VMs are isolated per mod
and stage, host APIs are permissioned and schema-checked, boundary values are data-only, and memory,
time, instruction, stack, source, event, and error budgets fail closed. Builds can disable Luau
while preserving metadata validation.

## Built applications

The default build defines these applications:

- `heartstead_dev_game`
- `heartstead_dedicated_server`
- `heartstead_asset_lab`
- `heartstead_render_smoke`
- `heartstead_render_benchmark`
- `heartstead_audio_benchmark`
- `heartstead_ui_benchmark`
- `heartstead_scripting_benchmark`

Focused samples cover platform, renderer, physics, networking, scripting, jobs, math, mods,
workpieces, chunks, rooms, world state, and related boundaries. Tools cover asset cooking, shader
compilation, mod validation, block models, chunks, logs, profiles, prototypes, replay, saves,
workpieces, and world inspection. Use the build system rather than this page to enumerate exact
current targets:

```bash
cmake --build --preset default-debug --target help
```

## Current constraints

- **Gameplay:** foundation systems exist, but the authored survival, settlement, progression,
  crafting, ecology, and world-generation experience is not complete.
- **Platform:** Linux/X11 is the complete native window and Vulkan presentation path. Headless code
  is portable; wider native window/presentation support remains future work.
- **Networking:** numeric IPv4 only; no DNS discovery, encrypted identity, NAT traversal,
  matchmaking, or public-service hardening.
- **Dedicated persistence:** the dedicated executable is memory-only.
- **Renderer:** the current frame graph, resource set, and material binding model are intentionally
  bounded rather than general-purpose. Local shadowing is budgeted to two spotlight maps; point
  lights support direct lighting, but their omnidirectional shadow path is not enabled.
- **Assets:** use the [asset pipeline guide](asset_pipeline.md) as the exact supported-format
  contract; do not infer support from what a third-party glTF exporter can produce.
- **Compatibility:** schemas and binary payloads are versioned, but the project is pre-release and
  may still require explicit migrations between development revisions.

## Verification

Verify the maintained baseline through CMake/CTest presets rather than relying on a frozen
test-count claim:

```bash
cmake --preset default-debug
cmake --build --preset default-debug
ctest --preset default-debug
```

Warning-as-error, compiler-specific, AddressSanitizer/UndefinedBehaviorSanitizer, LeakSanitizer, and
ThreadSanitizer presets are available. See [Testing](dev/testing.md) for the expected verification
matrix and [Renderer benchmarks](performance/renderer_benchmarks.md) for reproducible performance
measurement.
