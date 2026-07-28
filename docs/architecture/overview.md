# Heartstead Architecture Overview

Heartstead is a custom-engine co-op voxel settlement survival game where the settlement
is the real progression system.

The engine must keep several world representations separate from day one:

```text
World voxels   = terrain and editable world mass
Build pieces   = placed settlement construction
Assemblies     = logical multiblock structures made from build pieces
Workpieces     = small local microvoxel crafting objects
Entities       = players, creatures, animals, carts, dropped objects
Items          = inventory/storage objects
Cargo          = large, bulk, unstable, or physical transport objects
Networks       = roads, storage access, power, wards, water, smoke, logistics
Rooms          = derived environmental spaces and descriptors
Processes      = long-running production and transformation state
```

These systems can interact and convert into each other, but they must not share one
storage model.

## Engine/Game/Mod Boundary

The vanilla game is the first official mod.

```text
Native Engine
  performance, platform, rendering, physics integration, chunks, networking,
  save database, asset loading, mod runtime, scripting sandbox, invariants

Game Runtime
  settlement rules, crafting rules, rooms, progression, ecology, combat,
  farming, animals, logistics, magic behavior

Base Mod
  vanilla items, recipes, voxels, build pieces, workpieces, creatures, crops,
  animals, biomes, machines, room descriptors, resources, assets
```

No gameplay system should get a private shortcut that mods cannot use later unless
there is a strong reason.

## Current Vertical Slice

The repository currently implements the initial engine-owned foundation:

- C++23 CMake library target with warning-as-error and Clang sanitizer presets
- stable identity primitives
- shared stable 64-bit hash helper for content hashes, prototype fingerprints, cooked payload
  integrity checks, shader manifest identity, and deterministic derived runtime ids
- logging/assert support
- shared math primitives for vectors, transforms, and bounds
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
- backend-neutral audio event/mixer boundary with validated sound-event prototypes, exact
  floating-origin spatial math, deterministic buses/voice stealing, and a headless null backend
- shader compiler tool boundary with development validation backends and production SPIR-V
  passthrough for active shader assets
- renderer RHI boundary with backend/device capabilities, material definitions, render frame
  plans with renderer-neutral resource-use/dependency/state-transition execution planning,
  headless smoke backend, optional Vulkan offscreen and surface-backed clear smoke backend with
  private synchronization translation, offscreen all-resource barrier submission, and
  planned-vs-submitted barrier accounting, renderer-owned buffer upload handles, private Vulkan
  shader-module creation, material pipeline layout binding, private Vulkan descriptor set
  layout/pipeline layout/descriptor set allocation, private Vulkan compute and graphics pipeline
  creation, RGBA8 sampled image upload, uniform and sampled texture descriptor writes, mesh draw
  binding with minimal offscreen Vulkan draw-command submission, and optional X11 surface ownership
  boundary
- physics world boundary with backend capabilities, deterministic headless integration, and a
  pinned Jolt rigid-body backend with compounds, response, sleeping, AABB queries, and contacts
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
- script module materialization from lifecycle-classified mod script files into validated module
  descriptors with declared permissions and API versions
- mod manifest discovery
- mod manifest dependency ordering
- engine-owned, inspectable mod lifecycle plan for settings, prototype, data update,
  final fix, asset/resource, migration, runtime server, and runtime client stages
- generic prototype loading
- generic prototype patch data-update and final-fix stages
- deterministic mod prototype fingerprints for save compatibility metadata, including patches
- content-validation-driven new-save metadata generation from active mod prototype fingerprints
- material prototype loading into renderer material definitions
- material asset-reference validation against active asset catalog records
- scenario definition materialization and selected-scenario game runtime startup validation
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
- versioned save metadata and stable save id allocation
- save compatibility checks against active mod prototype fingerprints
- save migration registry and ordered migration runner
- typed save snapshot text persistence
- typed binary save snapshot persistence
- file-backed save database for full snapshots and chunk deltas
- CTest coverage for unit tests plus headless-safe sample and tool smoke tests
- timestamped processes
- simulation LOD planning for full, simplified, sleeping, and unloaded subjects
- world-state simulation subject derivation from entity, build-piece, assembly, process-owner,
  derived network-node, and terrain chunk-region transforms
- dirty-region rebuild scheduling for chunks, rooms, and networks
- spatial networks
- room graph metrics and descriptors
- flood-fill room extraction from terrain voxel and build-piece source inputs
- transactional world operation journal with inspectable stages, effects, and dirty flags
- item stack and cargo records
- entity runtime identity records
- entity transforms persisted through save snapshots and accepted by authoritative spawns
- compound physical-resource lifecycle from one physics body into cargo
- build-piece and assembly validation records plus validated direct/staged assembly construction
- world state container with separate runtime stores for regions, chunks, build pieces, entities,
  cargo, inventories, workpieces, physical resources, assemblies, processes, fires, rooms, networks,
  mod state, missing-prototype placeholders, and the persisted voxel-palette manifest
- world snapshot bridge between runtime ownership and typed save sections
- prototype registry lookup and expected-kind validation
- game runtime target scaffold above the engine
- game runtime startup from aggregate content validation
- game-owned default script host API registry for server/client/migration scripting stages,
  validated by the engine scripting boundary
- typed script host API payload schemas and emitted-event argument validation before native command
  routing
- game-owned script host command routes that convert selected runtime-server script events into
  server-authoritative command envelopes
- routed script host command dispatch through the authoritative command dispatcher with
  per-command success/error reports
- server-authoritative command dispatch skeleton
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
  result intake, replication intake with duplicate/out-of-order replication rejection, and
  server-disconnect cleanup
- replication event batch codec, deterministic interest filtering, and in-memory host delivery path
- world-derived replication interest rules from simulation subjects and viewer positions, with
  inspectable derivation reports and host-session policy refresh for live ticks
- world-layer replication delta planning that classifies event subjects, including workpieces,
  across concrete world stores before typed snapshot/delta serialization
- typed replication delta materialization using existing save-section record shapes through the
  current text transport payload
- world-layer host-tick delta materialization from authoritative command reports into typed
  snapshots, with explicit skipped-command diagnostics, error details, and rollback trace summaries
- world-layer typed replication delta apply through separate stores, preserving receiving-side
  runtime/session identity for persistent entities
- world-layer client replication apply adapter from queued event intake to matched typed deltas
  without moving world-store knowledge into client protocol code
- world-owned typed delta transport payloads over reliable replication envelopes
- world-layer typed delta delivery reports that route complete typed snapshots to relevant host
  clients while leaving transport/session code payload-agnostic
- client-side raw replication-envelope intake for non-event payloads, with world-layer typed delta
  draining and apply over the existing event-batch matching path
- command replay log and runner foundation with optional deterministic expectation checks for
  committed mutations, events, reserved ids, dirty flags, and operation traces
- structured debug inspection for backend capability records, transaction records, and core records
- structured debug inspection for room records, spatial networks, dirty-region rebuild queues, and
  mod lifecycle plans
- structured debug inspection for script module records, script host API descriptors, script host
  event batches, transport server welcome handshakes, accepted client transport sessions,
  host-session command results, world replication delta plans and snapshots, and client protocol
  sessions
- structured debug inspection for simulation LOD policies, per-subject LOD decisions, and
  frame-plan summaries
- game-side structured inspection for script host command routes, dispatch reports, and dispatch
  batches
- replay inspector tool
- save inspector tool
- workpiece inspector tool

The active implementation sequence and acceptance budgets are tracked in
[`docs/roadmap/gameplay_foundations_m1_m8.md`](../roadmap/gameplay_foundations_m1_m8.md).
The broader next engine slices remain:

1. production Luau VM binding behind the registered scripting host API contract
2. proven-library external networking backend beyond the current POSIX UDP reliability foundation
3. production binary media converter implementations behind the asset cooker backend slot
4. Vulkan production render-pass/subpass execution over planned frame resources
5. broader native platform coverage beyond the current X11 window/input/display/clipboard backend
