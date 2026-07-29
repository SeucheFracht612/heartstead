# Game Runtime Architecture

The game runtime is the Heartstead-specific layer above the native engine.

Implemented foundation:

- `Heartstead::GameRuntime` CMake target
  - links against `Heartstead::Engine`
  - does not own engine infrastructure

- `GameRuntimeConfig`
  - base mod id
  - base mod requirement
  - selected scenario prototype id
  - selected scenario requirement
  - required prototype kinds
  - default script host API registration
  - additional script host APIs for future game/runtime integrations
  - default script host command route registration
  - additional script host command routes for future authoritative actions

- `GameRuntime`
  - initializes from an engine `ModValidationReport` for server/headless prototype checks
  - initializes from `ContentValidationReport` when full mod/resource/asset validation is required
  - rejects startup when mod or aggregate content validation has errors
  - requires the official base mod by default
  - requires `base:scenarios/homestead` by default
  - verifies that the selected scenario exists and is a `scenario` prototype
  - requires the selected scenario's materialized `ScenarioDefinition` when started from
    aggregate content validation
  - records the selected scenario start region and spawn mode in the startup report
  - materializes the selected scenario again at session creation, records its identity in
    authoritative state, and applies its new-world spawn area and cargo bootstrap exactly once
  - grants scenario starting items once for each newly created persistent player inventory, while
    preserving an existing inventory when a saved player reconnects
  - restores the saved scenario automatically and rejects explicit save/scenario mismatches
  - restores saved voxel type assignments into a session-owned palette, appends newly introduced
    prototypes, and represents removed prototypes with named missing-voxel definitions
  - verifies that required engine-level prototype families are present, including materials
    and scenarios
  - registers the default Heartstead script host API surface for server runtime, client runtime,
    and migration stages
  - validates additional configured script host APIs through the engine scripting registry
  - registers and validates default server-side script host command routes for translating host
    events into command envelopes
  - creates a `ScriptRuntimeDesc` with the game-approved host API registry for the selected
    scripting backend
  - creates a `ScriptHostCommandRouter` with the game-approved event-to-command mappings
  - dispatches routed runtime-server script host events through the existing authoritative command
    dispatcher and reports per-command success, world mutation, emitted events, reserved ids, and
    errors
  - exposes game-side inspection records for script host command routes, reports, and batches
  - records the aggregate voxel palette entry count when started from full content validation
  - records resource-pack, active-asset, material-definition, and material-reference counts when
    started from aggregate content validation
  - registers game-owned system descriptors

- `GameSystemDescriptor`
  - names Heartstead-specific systems such as building, rooms, crafting, production,
    logistics, farming, animals, ecology, combat, sleep, magic, and workpieces
  - records whether a system is server-authoritative and mod-data-driven

The runtime now also composes a fixed-tick local server/client session, typed gameplay modules,
generation-safe entity lifecycle, player movement, authoritative voxel interaction, replication,
retained client presentation, save/reload, and a headless session harness. The normative ownership
and lifecycle contract is in [`runtime_composition.md`](runtime_composition.md).

Gameplay module serializer, persistence, replication, and presentation registrations are active
runtime contracts. Persistence callbacks participate in snapshot capture/restore, replication
callbacks consume typed client events, and presentation callbacks update retained proxies. Live
session inspection reports composition, world/entity/chunk counts, module counts, per-system
timings, client queue state, presentation state, and terminal frame faults.

The layer boundary remains:

```text
engine/
  reusable native infrastructure and invariants

game/
  Heartstead-specific rules and system orchestration

mods/base/
  vanilla content loaded through the same mod path as community mods
```

Future gameplay rules should be added under `game/` and should consume engine systems
through public APIs. Vanilla content should continue to come from `mods/base/`, not from
private runtime shortcuts.

The default script host API registry is game-owned, not hardcoded into the scripting backend.
The engine validates IDs, stages, API versions, and permissions, while the game runtime decides
which Heartstead actions scripts may request. Those host API events remain intake records until
authoritative game systems choose to turn them into commands, transactions, diagnostics, or
replication.

Server-side script host command routes are also game-owned. They map selected runtime-server host
API events, such as `world.set_voxel`, `workpiece.edit_cell`, and `process.start`, into command
envelopes with deterministic payload fields. The existing server command dispatcher still validates
and applies the mutation, so scripts never receive direct world, save, network, renderer, or
physics handles.

The router can dispatch those command envelopes immediately through `ServerCommandDispatcher`.
Failures are reported per script event instead of mutating the world silently. This keeps the
authoritative command path, transaction journal, save dirtiness, and replication events as the only
way script-requested gameplay mutations enter the world state.

`GameInspector` emits the same `InspectionData` shape as engine debug inspection for game-owned
script command routes and dispatch reports. This avoids an engine dependency on `game/` while
keeping the script-to-command bridge visible to tools and future overlays.
