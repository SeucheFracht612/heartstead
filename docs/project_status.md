# Project status

This page records the maintained implementation status of Heartstead. It is not a roadmap or a
promise that every architecture target is complete.

**Audit baseline:** repository state documented and validated on 2026-08-01.

## What works now

### Player-facing game shell

`heartstead` is the primary executable. It owns the application window, renderer, UI, audio,
input, jobs, settings, menus, asynchronous session loading, and an optional unified runtime
session. It supports persistent new/load/continue/host flows, direct-address clients, data-driven
developer worlds, local and multiplayer pause behavior, return-to-menu teardown, automatic CLI
launches, model presentation, authoritative movement and voxel editing, and F3 lifecycle/resource
diagnostics. Local single-player is always an authoritative server plus a client over in-memory
transport.

### Standalone development diagnostic

`heartstead_dev_game` composes an authoritative server and a client in one process for local play.
It can also run as a remote client against `heartstead_dedicated_server`. The development scene
exercises voxel editing, movement, camera modes, inventory and map UI, models, animation, audio,
particles,
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

### Performance and profiling

The deterministic benchmark family retains raw samples and percentile summaries. Renderer schema
v4, chunk-streaming schema v2, voxel-response schema v1, and chunk-render-readiness schema v1 record
source/build/machine/device provenance and enforce workload-specific absolute gates. Clean
reference runs cover renderer/edit workloads, generated and indexed saved-delta publication, exact
collision publication through headless and Jolt physics, complete resident-field relighting, and
required generated chunks through current mesh residency, upload, visibility filtering, and exact
draw-command construction on headless and Vulkan devices. An optimized `profiling-release` preset
links on-demand Tracy instrumentation across the main runtime, renderer, worker, chunk, lighting,
collision, and streaming boundaries; normal builds compile those call sites to no-ops. Physical
disk/cache and persistence-scale workloads, general-controller loader adoption, actual GPU
execution/presentation timing, and multiplayer scale remain staged in the
[voxel optimization roadmap](performance/voxel_optimization_roadmap.md).

The retained UI path uses a packaged Noto Sans font rendered from a deterministic SDF atlas,
strict UTF-8 decoding, DPI-scaled widgets, hierarchical clipping/scissors, nine-slice panels,
atlased icons, inventory previews, accessibility color transforms, and exposure-independent final
composition. The game includes a discovered-area minimap and a full map with surface,
underground, aerial, and mod-defined layers plus gameplay-owned markers.

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

The player-facing game can create and reopen a versioned save database, including world data,
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

- `heartstead`
- `heartstead_dev_game`
- `heartstead_dedicated_server`
- `heartstead_asset_lab`
- `heartstead_render_smoke`
- `heartstead_render_benchmark`
- `heartstead_voxel_benchmark`
- `heartstead_voxel_meshing_benchmark`
- `heartstead_chunk_streaming_benchmark`
- `heartstead_chunk_render_readiness_benchmark`
- `heartstead_voxel_response_benchmark`
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
- **Renderer:** local shadowing uses a bounded budget and point lights support direct lighting, but
  their omnidirectional shadow path is not enabled. The current anti-aliasing path is FXAA rather
  than temporal AA, so no temporal history image is active. Quality profiles expose the complete
  policy surface, but vegetation, water, particle, reflection, and asset-LOD values are not yet
  live-wired and the game has no player-facing quality selector. See
  [known renderer limitations](known_renderer_limitations.md) for the exact extension boundary.
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
measurement. The [Voxel response benchmarks](performance/voxel_response_benchmarks.md) document
the isolated collision/relight lifecycle gates and their exact headless/Jolt calibration boundary.
The [Chunk render-readiness benchmarks](performance/chunk_render_readiness_benchmarks.md) document
the required generated load-to-draw-command gate and its explicit pre-GPU-execution boundary.
