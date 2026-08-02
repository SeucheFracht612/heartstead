# Heartstead

Heartstead is an experimental C++23 engine and game foundation for a cooperative voxel
settlement-survival game. The project is building the technical base for editable cubic worlds,
server-authoritative multiplayer, persistent settlements, data-driven content, and a production
asset pipeline.

The repository currently contains a playable **development slice**, not a finished game. You can
walk through and edit a voxel world, exercise the character controller and presentation systems,
run local or direct-IP multiplayer, save a local world, and validate the engine's content,
rendering, scripting, networking, and persistence boundaries.

## Current capabilities

The maintained development slice includes:

- signed 64-bit world coordinates and cubic chunks;
- editable voxel terrain, asynchronous meshing, lighting, fluids, and large-coordinate rendering;
- first- and third-person movement backed by Jolt Physics;
- a Vulkan 1.3/X11 renderer with dynamic rendering, HDR, PBR lighting, bounded post-processing,
  directional and local spotlight shadows, plus a deterministic headless backend;
- glTF Model v5 and role-aware texture cooking, retained model/animation presentation, visual
  prefabs, particles, audio, UI, debug overlays, and the Asset Lab inspector;
- profile-driven atmosphere, clouds, fog, weather, water, vegetation, trails, and surface marks;
- data-driven prototypes, mods, resource packs, and sandboxed Luau scripting;
- server-authoritative commands, replication, client prediction, and direct numeric-IPv4 UDP;
- versioned saves, migrations, profiles, discovery, logs, replay, and inspection tools;
- broad unit, integration, smoke, sanitizer, and benchmark coverage.

Important current limits:

- the scene is an engine integration playground rather than the intended survival/settlement
  gameplay loop;
- the complete native presentation path is currently Linux/X11 with Vulkan;
- direct-IP multiplayer is for controlled LAN/testing use and provides neither encryption nor
  account authentication;
- the standalone dedicated-server executable currently runs an in-memory world and has no save
  option;
- some asset and renderer extensions remain intentionally deferred; see the current status and
  asset-pipeline guides for the exact boundary.

See [Project status](docs/project_status.md) for the audited implementation snapshot.

## Quick start

Heartstead uses CMake presets, Ninja, and vcpkg manifest mode. CMake 3.25 or newer and a C++23
compiler are required.

```bash
export VCPKG_ROOT=/absolute/path/to/vcpkg
cmake --preset default-debug
cmake --build --preset default-debug
ctest --preset default-debug
```

Build and run the player-facing Heartstead application:

```bash
cmake --build --preset default-debug --target heartstead
./build/default-debug/apps/heartstead/heartstead
```

It opens at the main menu. New saves, existing saves, hosted saves, direct-IP clients, and
developer worlds and command-line launches all enter the same session-launch path. The standalone
`heartstead_dev_game` target remains only for focused presentation diagnostics.

Run the focused Renderer V2 checks:

```bash
ctest --test-dir build/default-debug -R \
  'renderer|vegetation|water|environment_effects|particle|animation|equipment|visibility|far_terrain|streaming_residency|ui_font|map_view|visual_regression' \
  --output-on-failure

./build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --scene starting-biome --headless --warmup 5 --frames 30 --radius 1
```

Native Vulkan, Asset Lab, visual-regression, and manual UI/map procedures are in
[Testing](docs/dev/testing.md).

Inspect a production-cooked visual through Asset Lab:

```bash
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --prefab base:visuals/player --preview character --lighting overcast
```

Run a dedicated server and join it from another process or machine:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server \
  --bind 0.0.0.0:7777

./build/default-debug/apps/heartstead/heartstead \
  --connect 192.168.1.10:7777
```

The client accepts numeric IPv4 endpoints. The dedicated server is headless and memory-only at
this stage. Do not expose the current transport to untrusted Internet clients.

Primary game controls:

| Action | Default input |
| --- | --- |
| Move | `W`, `A`, `S`, `D` |
| Jump / traversal | `Space` |
| Sprint | `Left Shift` |
| Crouch | `Left Ctrl` |
| Dash / roll | `Q` / `Left Alt` |
| Remove / place voxel | Left / right mouse button |
| Runtime diagnostics | `F3` |
| Close menu / pause | `Escape` |

The standalone presentation diagnostic retains additional inventory, map, camera, environment,
and geometry-debug controls described in [Running Heartstead](docs/dev/running.md).

The executable's `--help` output is the source of truth for command-line options. See
[Running Heartstead](docs/dev/running.md) for saves, bounded runs, remote play, and the complete
control notes.

## Documentation

Start at the [documentation index](docs/README.md).

- [Project status](docs/project_status.md) — what is implemented and what is not.
- [Build instructions](docs/dev/build_instructions.md) — dependencies, presets, and configuration.
- [Running Heartstead](docs/dev/running.md) — applications, controls, saves, and multiplayer.
- [Testing](docs/dev/testing.md) — test presets, sanitizers, smoke tests, and change verification.
- [Voxel optimization roadmap](docs/performance/voxel_optimization_roadmap.md) — measured staged
  work from profiling through storage, streaming, scale, and trace-gated GPU techniques.
- [Chunk render-readiness benchmarks](docs/performance/chunk_render_readiness_benchmarks.md) —
  generated interest through exact mesh upload and production draw-command eligibility.
- [Predictive streaming benchmarks](docs/performance/predictive_streaming_benchmarks.md) — paired
  baseline/predictive traversal, cancellation, visible-hole, and bounded-residency evidence.
- [Multiplayer chunk-subscription benchmarks](docs/performance/multiplayer_chunk_subscription_benchmarks.md)
  — eight-client relevance, hot-edit, backlog, and conditioned queue/private-memory soak evidence.
- [Multiplayer network-impairment benchmarks](docs/performance/multiplayer_network_impairment_benchmarks.md)
  — deterministic RTT/loss prediction, correction, bandwidth, backlog, and server-P99 evidence.
- [Asset pipeline](docs/asset_pipeline.md) — contributor formats, importing, cooking, and limits.
- [Asset Lab](docs/asset_lab.md) — production asset and presentation inspection.
- [Environment rendering](docs/architecture/environment_rendering.md) — atmosphere, water,
  vegetation, weather, and effects.
- [Architecture overview](docs/architecture/overview.md) — system boundaries and data flow.
- [Game executable](docs/architecture/game_executable.md) — application lifetime and state machine.
- [Game front end](docs/architecture/game_front_end.md) — menu, saves, and application settings.
- [Engine specification](docs/architecture/engine_spec.md) — normative long-term contract.

Architecture decision records live in [`docs/adr`](docs/adr). Detailed subsystem documents live in
[`docs/architecture`](docs/architecture).

## Repository layout

```text
apps/       runnable development, server, smoke, and benchmark applications
engine/     reusable engine infrastructure and public boundaries
game/       Heartstead-owned runtime composition and gameplay-facing systems
mods/base/  base content loaded through the public mod/prototype pipeline
samples/    focused executable examples and sandboxes
tests/      unit, integration, regression, and fixture coverage
tools/      cookers, validators, inspectors, and diagnostics
docs/       maintained architecture, contributor, and operational documentation
```

Use the build system to enumerate available targets for the selected preset:

```bash
cmake --build --preset default-debug --target help
```

## Architecture laws

```text
Terrain voxels are not crafting voxels.
Build pieces are not terrain blocks.
Assemblies are logical machines made from pieces.
Rooms are derived descriptors, not hand-authored boxes.
Networks connect settlement systems.
Processes advance from authoritative world time, not per-frame hacks.
Cargo is not just an inventory stack.
Mods define game meaning.
The server owns meaningful state.
Saved identity is stable and versioned.
Presentation never owns authoritative state.
```

Documentation describes current behavior only when it is verified by code or tests. Completed
milestone reports belong in version history; maintained pages describe durable contracts,
workflows, status, and reproducible benchmarks.
