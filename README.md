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
- a Vulkan/X11 native renderer plus a deterministic headless renderer for tests and tools;
- glTF model cooking and runtime rendering, animation, particles, audio, UI, and debug overlays;
- data-driven prototypes, mods, resource packs, and sandboxed Luau scripting;
- server-authoritative commands, replication, client prediction, and direct numeric-IPv4 UDP;
- versioned saves, migrations, profiles, discovery, logs, replay, and inspection tools;
- broad unit, integration, smoke, sanitizer, and benchmark coverage.

Important current limits:

- the scene is an engine integration playground rather than the intended survival/settlement
  gameplay loop;
- the complete native presentation path is currently Linux/X11 with Vulkan;
- direct-IP multiplayer is for controlled LAN/testing use and provides neither encryption nor account authentication;
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

Run the interactive local development game:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game
```

Run a dedicated server and join it from another process or machine:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server \
  --bind 0.0.0.0:7777

./build/default-debug/apps/dev_game/heartstead_dev_game \
  --connect 192.168.1.10:7777
```

The client accepts numeric IPv4 endpoints. The dedicated server is headless and memory-only at
this stage. Do not expose the current transport to untrusted Internet clients.

Common controls:

| Action | Default input |
| --- | --- |
| Move | `W`, `A`, `S`, `D` |
| Jump / traversal | `Space` |
| Sprint | `Left Shift` |
| Crouch | `Left Ctrl` |
| Dash / roll | `Q` / `Left Alt` |
| Interact | `E` |
| Inventory | `Tab` |
| Remove / place voxel | Left / right mouse button |
| Camera / diagnostics / geometry | `F1` / `F3` / `F4` |
| Close menu / pause | `Escape` |

The executable's `--help` output is the source of truth for command-line options. See
[Running Heartstead](docs/dev/running.md) for saves, bounded runs, remote play, and the complete
control notes.

## Documentation

Start at the [documentation index](docs/README.md).

- [Project status](docs/project_status.md) — what is implemented and what is not.
- [Build instructions](docs/dev/build_instructions.md) — dependencies, presets, and configuration.
- [Running Heartstead](docs/dev/running.md) — applications, controls, saves, and multiplayer.
- [Testing](docs/dev/testing.md) — test presets, sanitizers, smoke tests, and change verification.
- [Asset pipeline](docs/asset_pipeline.md) — contributor formats, importing, cooking, and limits.
- [Architecture overview](docs/architecture/overview.md) — system boundaries and data flow.
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
