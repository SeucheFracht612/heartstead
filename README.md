# Heartstead

Heartstead is a working-title custom-engine co-op voxel settlement survival game.

The settlement is the progression system. Engine work therefore starts with the
boundaries that keep terrain, construction, assemblies, workpieces, entities, items,
cargo, networks, rooms, and long-running processes from collapsing into one universal
block abstraction.

## Current Status

Heartstead now contains the engine foundation, Renderer V1, and a gameplay-ready runtime spine.
The development application runs an authoritative local server and replicated client in one
process; the dedicated server and headless test host use the same fixed-tick simulation path.

Implemented in this repository:

- CMake/Ninja project scaffold
- warning-as-error and Clang sanitizer build presets
- C++23 engine library target
- stable identity primitives
- diagnostics, logging, and asserts
- shared math primitives for vectors, transforms, bounds, and column-major camera/projection
  matrices
- job system boundary with immediate and thread-pool backends
- backend-selectable platform abstraction with backend capabilities, headless smoke backend,
  optional X11 native windows, opaque native window handles, validated events, and retained input
  state for keyboard, text, mouse buttons, pointer position, and wheel deltas, plus display
  metadata with optional XRandR monitor details, deterministic headless clipboard text, and X11
  clipboard ownership/retrieval with bounded large-transfer and `INCR` support
- virtual file system with namespaced paths
- resource pack discovery, deterministic load plans, and asset catalog metadata
- cooked asset manifest metadata and backend-selectable asset cooker with development
  passthrough plus partial production data-like, material, glTF/GLB model, PNG/KTX2/JPEG texture,
  SPIR-V shader, WAV/OGG/FLAC audio, and SFNT font converters with decoded production metadata
- cooked asset dependency validation and manifest-verified payload loading
- shader compiler tool boundary with development validation backends and production SPIR-V
  passthrough for active shader assets
- renderer RHI boundary with backend/device capabilities, material definitions, render frame
  plans plus pass-associated unified draw submissions, headless validation, explicit GPU terrain
  vertices, checked-in SPIR-V loading, push constants, configurable graphics state, and optional
  Vulkan rendering through offscreen color/depth targets into an X11 swapchain
- physics world boundary with backend capabilities, deterministic headless reference, and a pinned
  Jolt 5.6 backend for rigid bodies, compounds, full dynamic rotation, collision response,
  sleeping, AABB queries, contact snapshots, cooked voxel terrain, virtual character movement, and
  authoritative dropped-resource lifecycle synchronization
- backend-selectable network transport boundary with in-memory host implementation and a POSIX UDP
  foundation host for host-owned loopback client endpoints when sockets are available, plus
  endpoint config, reliable command sequence enforcement, packet fragmentation/reassembly,
  integrated deterministic reliable acknowledgement/retry/drop maintenance, and capability
  reporting
- host-session lifecycle for local authoritative command processing
- scripting runtime boundary with capability-gated calls, sandbox resource limits, disabled
  backend, restricted Luau foundation backend, and registered host API/event validation for
  emitted events and bounded return values, plus ordered script host event intake records for the
  authoritative runtime
- script module materialization from staged mod script files into validated descriptors with
  declared permissions and API versions
- base mod discovery from `mods/base`
- mod manifest dependency ordering
- engine-owned, inspectable mod lifecycle plan for settings, prototype, data update,
  final fix, asset/resource, migration, runtime server, and runtime client stages
- generic prototype loading for engine-level validation
- generic prototype patch data-update and final-fix stages
- deterministic mod prototype fingerprints for save compatibility metadata, including patches
- material prototype loading into renderer material definitions
- material asset-reference validation against active asset catalog records
- scenario definition materialization plus authoritative new-world startup, saved-scenario
  recovery, spawn/loadout/cargo application, and mismatch rejection
- item and cargo definition materialization from content prototypes
- entity definition materialization from dynamic object prototypes
- voxel palette construction from terrain voxel prototypes
- assembly definition materialization from multiblock prototypes
- process definition materialization from timestamped process prototypes
- workpiece definition materialization from local microvoxel prototypes
- aggregate content validation for mods, resource packs, assets, and material references
- mod validator and prototype inspector tools
- semantic validation for engine-owned prototype fields
- region graph for broad world structure separate from streamed chunks
- deterministic terrain generator from seed, region descriptors, and voxel palette
- renderer-neutral chunk surface mesh extraction
- cubic terrain chunks with signed 64-bit chunk coordinates and 64-bit-safe command/save paths
- canonical signed 64-bit block/chunk conversions with negative-safe floor division and checked
  chunk/local arithmetic
- chunked terrain voxel storage with dirty-state tracking
- local workpiece grid storage with operation history
- workpiece template matching and grid text codec
- versioned save metadata and save id reservation
- save compatibility checks against active mod prototype fingerprints
- save migration registry and ordered migration runner
- timestamped process runtime foundation
- one persisted authoritative `u64 world_time` value with checked advancement and save migration
- simulation LOD planner for full, simplified, sleeping, and unloaded subjects
- dirty-region rebuild scheduling for chunks, rooms, and networks
- generic spatial network graph foundation
- room graph metrics and descriptor evaluation
- flood-fill room extraction from terrain voxel and build-piece source inputs
- transactional world operation journal with inspectable stages, effects, and dirty flags
- item stack and cargo representations
- entity runtime/save identity separation
- compound physical-resource lifecycle from dynamic body to cargo
- build-piece prototype materialization, derived invalidation, assembly representations, and
  validated direct/staged assembly construction
- prototype registry validation
- game runtime target above the engine
- composition-root runtime with local server/client and dedicated-server modes
- fixed-phase simulation scheduling with typed tick event streams
- generation-safe entity/component lifecycle and replicated destruction tombstones
- authoritative player movement plus validated place/remove voxel commands
- distinct replicated client, retained presentation, and immutable render-snapshot state
- executable gameplay module registration with typed domain-service, persistence, client
  replication, and retained-presentation interfaces
- authoritative save/reload with feature capture/restore hooks and a reusable headless session
  integration harness
- live runtime diagnostics for composition, world/client/presentation state and per-system timings,
  with terminal fault capture after partially executed frame synchronization
- game runtime startup from aggregate content validation
- game-owned default script host API registry for server/client/migration scripting stages,
  validated by the engine scripting boundary
- typed script host API payload schemas and emitted-event argument validation before native command
  routing
- game-owned script host command routes that convert selected runtime-server script events into
  server-authoritative command envelopes
- routed script host command dispatch through the authoritative command dispatcher with
  per-command success/error reports
- versioned save metadata text codec
- typed save snapshot text codec
- typed save snapshot binary codec
- file-backed save database for full snapshots and chunk deltas
- typed save snapshot sections with prototype/reference validation
- chunk database with voxel edit records and neighbor invalidation
- world state container with separate stores for regions, chunks, build objects, entities, cargo,
  inventories, workpieces, physical resources, assemblies, processes, fires, rooms, networks, mod
  state, missing-prototype placeholders, and the persisted voxel-palette manifest
- world snapshot bridge for exporting/importing typed save sections from runtime world state
- server-authoritative command dispatcher skeleton
- deterministic command payload codec for structured engine command fields
- engine-owned world command handlers for voxel edits and sleep, build-piece placement/completion,
  local workpiece edits/finishing, inventory transfers, persistent cargo creation, entity spawns,
  timestamped process starts/advancement, and direct or staged assembly lifecycle operations
- deterministic transport packet codec and packet fragmentation/reassembly used by the current
  project-owned POSIX UDP backend
- deterministic transport control payload codec with server welcome messages for session
  handshakes and server disconnect messages for graceful session close
- client-side transport handshake acceptance into validated session records
- deterministic host-session command-result payload codec and client-side response validation
- client-side protocol session state for command sequencing, pending command tracking, command
  result intake, inspectable replication intake reports, duplicate/out-of-order replication
  rejection, and server-disconnect cleanup
- replication event batch codec, deterministic relevance filtering, and in-memory host delivery path
- world-derived replication interest rules from simulation subjects and viewer positions, with
  inspectable derivation reports and a world-layer host-session policy refresh helper for live ticks
- world-layer replication delta planning that classifies event subjects across build pieces,
  persistent entities, cargo, assemblies, owner inventories, workpieces, and owner processes without
  collapsing those stores into one replicated object model
- typed replication delta materialization that reuses existing save-section record shapes for the
  current text transport payload and future binary codecs
- world-layer host-tick delta materialization that converts successful authoritative mutating
  command reports into typed replication snapshots while reporting skipped commands explicitly
- world-layer typed replication delta apply that upserts separate world stores while preserving
  client-local persistent-entity runtime/session ids
- world-layer client replication apply adapter that drains queued client event intake and applies
  matching typed deltas without moving world-store knowledge into the net layer
- world-owned typed delta transport payloads over reliable replication envelopes
- world-layer typed delta delivery from host tick materialization to relevant clients through a
  payload-agnostic host replication send hook, skipping partial deltas that need resync
- client protocol intake for non-event replication payloads as raw envelopes so typed world deltas
  can be decoded and drained by the world layer without teaching networking about world stores
- deterministic text codec for typed replication delta snapshots, reusing save snapshot text for
  materialized sections while validating delta-plan counts
- server-side player profile persistence with stable UUIDs, display-name history, roles,
  spawn/bed state, markers, portable flags/settings, and layered map-discovery region bitsets
- server-side append/flush log storage for join, leave, and chat records with UTC and world
  timestamps, escaped fields, bounded current-plus-archive queries, and size/day rotation
- command replay log, codec, runner foundation, and replay inspector tool
- structured debug inspection data for backend capabilities, transactions, core records, room
  records, spatial networks, dirty-region rebuild queues, and mod lifecycle plans
- structured debug inspection data for script module records, script host API descriptors, script
  host event batches, transport server welcome handshakes, accepted client transport sessions,
  host-session command results and tick results, replication batches, network relevance reports,
  client-side replication intake reports, world-derived replication interest and delta reports,
  typed replication delta snapshots, and client protocol sessions
- game-side structured inspection data for script host command routes, dispatch reports, and
  dispatch batches
- platform, renderer, physics, network, scripting, jobs, math, and mod sandbox samples
- native Milestone 2 Vulkan application backed by a retained renderer, budgeted chunk mesh/upload
  queues, revision/generation-safe GPU cache, frustum culling, camera-relative multi-chunk indexed
  drawing, resize/minimize handling, validation callbacks, and clean shutdown
- Milestone 3 CPU zones, delayed Vulkan timestamp queries and debug labels, unified renderer
  counters, ten deterministic benchmark/stress scenes, percentile and low-FPS summaries, uncapped
  execution, and complete JSON/CSV frame export
- CTest coverage for unit tests plus headless-safe sample and tool smoke tests

Full game feature rules, remote-client composition, production Jolt/Luau integrations, and wider
native platform coverage remain future work. Renderer V1 and the local gameplay vertical slice are
implemented; new renderer work is driven by measured gameplay needs.

The normative target and the implementation audit are in
[`docs/architecture/engine_spec.md`](docs/architecture/engine_spec.md) and
[`docs/architecture/engine_v0_2_audit.md`](docs/architecture/engine_v0_2_audit.md). Runtime ownership
and feature extension are documented in
[`docs/architecture/runtime_composition.md`](docs/architecture/runtime_composition.md).

## Build

Configure:

```bash
cmake --preset default-debug
```

Build:

```bash
cmake --build --preset default-debug
```

Test:

```bash
ctest --preset default-debug
```

The default test preset runs unit tests and smoke tests for the default-built samples and tools.

Additional guardrail presets:

```bash
ctest --preset default-debug-werror
ctest --preset linux-clang-debug-werror
ctest --preset linux-gcc-debug-werror
ctest --preset linux-clang-asan
ctest --preset linux-clang-asan-leaks
ctest --preset linux-clang-tsan
```

The compiler-specific warning presets make the GCC/Clang matrix reproducible instead of depending
on the host default compiler. The sanitizer presets use Clang, treat warnings as errors, and keep
Vulkan disabled so they focus on engine foundation code.
The default ASan CTest preset disables LeakSanitizer startup checks for managed/debugger-style test
environments. `linux-clang-asan-leaks` runs the same binaries with leak detection enabled.

Useful sample executables after building:

```bash
./build/default-debug/samples/platform_smoke/heartstead_platform_smoke
./build/default-debug/samples/renderer_smoke/heartstead_renderer_smoke
./build/default-debug/samples/physics_sandbox/heartstead_physics_sandbox
./build/default-debug/samples/network_sandbox/heartstead_network_sandbox
./build/default-debug/samples/scripting_sandbox/heartstead_scripting_sandbox
./build/default-debug/samples/jobs_sandbox/heartstead_jobs_sandbox
./build/default-debug/samples/math_sandbox/heartstead_math_sandbox
./build/default-debug/samples/mod_sandbox/heartstead_mod_sandbox
./build/default-debug/samples/workpiece_sandbox/heartstead_workpiece_sandbox
./build/default-debug/samples/chunk_sandbox/heartstead_chunk_sandbox
./build/default-debug/samples/room_sandbox/heartstead_room_sandbox
./build/default-debug/samples/world_state_sandbox/heartstead_world_state_sandbox
```

Run the native Vulkan terrain milestone (requires X11 and a present-capable Vulkan device):

```bash
./build/default-debug/apps/render_smoke/heartstead_render_smoke
```

Use WASD and Space to move, hold the right mouse button to look, and press Escape or close the
window to exit. See [`docs/dev/build_instructions.md`](docs/dev/build_instructions.md) for shader
rebuild commands and validation details.

The interactive development game uses the same terrain renderer with Jolt-backed movement. Press
F5 to throw a physical log from the camera and watch its authoritative body settle:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game
```

Run the long-lived headless dedicated-server process:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server
```

It runs fixed ticks until `SIGINT` or `SIGTERM`. For a bounded smoke/test run, pass a positive tick
count, for example `--ticks 120`. The executable still defaults to the in-memory transport and has
no remote join path; long-lived process behavior does not yet make it a remotely joinable server.

Run an uncapped deterministic renderer benchmark (headless by default, add `--vulkan` for GPU
timestamps and a native window):

```bash
./build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --scene flat --warmup 120 --frames 1000 --output build/benchmarks/flat.json
```

Use `--list-scenes` to enumerate the static and stress workloads. The output includes every
per-frame renderer timing/counter plus median, p95, p99, maximum, 1% low, and 0.1% low statistics.

Useful tools after building:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker
./build/default-debug/tools/mod_validator/heartstead_mod_validator
./build/default-debug/tools/mod_validator/heartstead_mod_validator . --inspect
./build/default-debug/tools/log_viewer/heartstead_log_viewer <server-root> server
./build/default-debug/tools/map_profile_inspector/heartstead_map_profile_inspector <world-root> <player-uuid>
./build/default-debug/tools/prototype_inspector/heartstead_prototype_inspector
./build/default-debug/tools/replay_inspector/heartstead_replay_inspector tests/fixtures/sample_command_replay.txt
./build/default-debug/tools/save_inspector/heartstead_save_inspector tests/fixtures/minimal_save_snapshot.txt
./build/default-debug/tools/shader_compiler/heartstead_shader_compiler
./build/default-debug/tools/workpiece_inspector/heartstead_workpiece_inspector tests/fixtures/sample_workpiece_grid.txt
./build/default-debug/tools/world_inspector/heartstead_world_inspector tests/fixtures/minimal_save_snapshot.txt
```

## Architecture Laws

```text
Terrain voxels are not crafting voxels.
Build pieces are not terrain blocks.
Assemblies are logical machines made from pieces.
Rooms are derived descriptors, not hand-authored boxes.
Networks connect settlement systems.
Processes advance over time, not per-frame hacks.
Cargo is not just an inventory stack.
Mods define game meaning.
The server owns meaningful state.
Saved identity is stable and versioned.
```
