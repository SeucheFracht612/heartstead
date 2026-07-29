# Runtime composition and gameplay boundary

This document is the ownership contract for the executable game runtime. It complements the
engine-specific documents: engine code supplies reusable technology, while `game/` composes that
technology into an authoritative simulation and client presentation.

## Supported compositions

`GameRuntime` is the composition root and owns at most one `RuntimeSession`. A
`RuntimeConfiguration` selects the session shape:

| Mode | Server | Client | Transport | Renderer/audio |
|---|---:|---:|---|---|
| Development game | yes | yes | in-memory | application-owned |
| Single-player | yes | yes | in-memory | application-owned |
| Listen server foundation | yes | yes | in-memory today | application-owned |
| Dedicated server | yes | no | configuration-selected; executable defaults in-memory | none |
| Headless integration test | yes | optional | in-memory | none |

The development executable uses the same local server/client path as headless tests. The dedicated
server creates no platform window, renderer, audio, or presentation world. A remote-client-only
session is deliberately rejected until connection establishment for the external transport is
wired into `RuntimeSession`; the protocol and transport backend remain separate so this does not
change gameplay commands or systems.

The current `heartstead_dedicated_server` executable is a long-lived headless process by default. It
uses the in-memory transport and advances fixed ticks until `SIGINT` or `SIGTERM`; `--ticks N`
selects a positive bounded count for smoke tests and server-path validation. It is not remotely
joinable because the runtime still has no remote accept/identity/authentication path.

Applications own platform graphics and audio services. `GameRuntime` owns content references,
session lifetime, the fixed-step clock, and the local runtime composition. It never exposes Vulkan
objects.

## Authoritative frame path

The implemented local path is:

```text
platform events
  -> InputActionMap / PlayerInputFrame
  -> ClientRuntime command envelope
  -> in-memory transport
  -> ServerRuntime command gateway
  -> fixed SimulationScheduler phases
  -> WorldState / EntityWorld mutation
  -> sealed TickEvents and replication deltas
  -> ClientRuntime replicated state
  -> PresentationWorld synchronization
  -> immutable RenderSnapshot
```

`ServerRuntime` is the sole commit owner for authoritative state. Command handlers receive a
bounded `CommandExecutionContext` and `WorldOperation`; clients, presentation, and render code do
not receive mutable server stores. Fixed phases are commands, movement, physics, world operations,
gameplay, environment, derived state, finalize, and replication. Systems declare phase and `after`
dependencies, and startup rejects missing dependencies or cycles.

The current application coordinates simulation and rendering from the platform thread. This is an
intentional first implementation of the ownership rules, not permission for worker jobs to mutate
the world. Renderer chunk workers already consume immutable snapshots. Future simulation/render
threads must preserve the same command, event, and snapshot boundaries.

## Commands, events, and entity lifecycle

Commands request an operation; events describe a completed operation. Reliable typed voxel and
player-input commands travel through the same host dispatcher used by headless tests. Validation
occurs before mutation. Failed commands return typed error codes and cannot silently dirty save or
replication state.

`TickEvents` owns explicit typed streams for entity creation/destruction, voxel changes, inventory
changes, and character movement. Streams are writable only during their tick and sealed before
consumers observe them. There is no unrestricted global event bus.

`EntityWorld` owns generation-safe IDs, typed sparse component stores, activation/sleeping state,
pending destruction, cleanup callbacks, and tombstones. Finalization removes all registered
components and emits one destruction event. Player removal is replicated as a reliable tombstone;
the client deletes the replicated movement record and the presentation synchronizer removes the
corresponding generation-safe proxy.

Voxels remain in `ChunkDatabase` and are not ECS entities. Chunk instance generations and content
revisions distinguish reloads and edits. Dirty regions and immutable meshing requests drive the
renderer pipeline described in `chunk_meshing.md` and `renderer_v1_handoff.md`.

## Client and presentation ownership

The client representations are separate:

- `ClientRuntime` owns accepted replicated chunks, player snapshots, a bounded 256-entry recent
  command-result window, and protocol state. It owns no GPU resources; overflow drops the oldest
  diagnostic result and is reported in frame statistics.
- `PresentationWorld` retains client-only object proxies by replicated source ID, remembers previous
  and current exact transforms, and applies revision checks and explicit removals.
- `RenderSnapshot` is an immutable-by-ownership value extracted for a frame. It contains exact
  `WorldPosition` values and prototype asset references, not Vulkan or RHI handles.
- `Renderer` owns GPU assets, caches, draw construction, and submission. Only the camera-relative
  delta is converted to float.

The Foundation diagnostic overlay consumes these public boundaries rather than backend internals.
It reports authoritative and presentation ticks, the last command/rejection, local prediction
movement diagnostics, the supporting voxel, player collision token, and authoritative collision
cook queue plus support-chunk content/cooked revisions. This keeps a failed edit or stale terrain
body diagnosable from the running game.

The development application still uses the renderer's V1 chunk synchronization entry point for
terrain cell snapshot construction. Dynamic presentation objects use the retained scene boundary.
Replacing terrain synchronization with chunk presentation change sets does not alter the RHI or
meshing worker contracts.

## Gameplay module extension

Features implement `IGameplayModule` and are supplied in `RuntimeConfiguration::gameplay_modules`.
At startup each module may:

1. validate the immutable prototype registry;
2. register typed component metadata;
3. register typed domain-service interfaces;
4. register command handlers;
5. register fixed-phase systems;
6. register versioned serializers;
7. register typed persistence capture and restore callbacks;
8. register replication payload contracts and client apply callbacks;
9. register presentation synchronization callbacks.

The callbacks are executable boundaries, not labels. Client replication dispatch invokes the
registered feature handler after world-owned records are applied, and presentation synchronization
invokes every retained feature adapter. Registration rejects persistence, replication, or
presentation entries that do not name a real handler. All names and interface types are duplicate
checked. The complete registration report is available from `ServerRuntime::gameplay_modules()`
for diagnostics. The built-in voxel interaction feature is registered through this contract rather
than special-cased command installation.

Cross-feature calls use `DomainServiceRegistry`. A feature registers a narrow interface type and
other features request that type; they do not reach into private component arrays or domain stores.
Declarative vanilla content remains in `mods/base` and is loaded through the same immutable content
pipeline as other mods.

## Persistence

`capture_save_snapshot()` exports only authoritative state. `save_to()` commits it through the
generation-staged file database, and `start_session_from_save()` validates the snapshot against the
active content before creating a session. Feature persistence callbacks can append or validate
typed snapshot state; restore callbacks run only after the authoritative stores have been imported.
A callback failure aborts capture or load with the registration name in the diagnostic.

For a new world, the selected materialized `ScenarioDefinition` records its id, start region, and
spawn mode in authoritative state, creates the starting terrain and cargo, and grants each new
player the declared starting item stacks. A save carries the scenario id and reload resolves it
automatically. Loading imports saved terrain, cargo, inventories, entities, and mod state rather
than applying the new-world bootstrap again, and rejects an explicitly conflicting scenario. The
client receives the resulting authoritative state through normal initial replication.

Render objects, GPU buffers, client interpolation, physics broadphase caches, and presentation
state are never saved. Disk writing is currently an explicit caller operation. Moving compression
and I/O behind immutable tick-boundary save inputs is the next persistence scheduling step and must
not grant background jobs mutable world access.

## Headless testing

`HeadlessSessionHarness` validates content, creates the normal `GameRuntime`, starts either a local
server/client or dedicated-server session, and advances a requested fixed-tick count without any
platform or graphics services. New features should test at least command validation, authoritative
commit, emitted events, replication/presentation where relevant, save reload, and teardown through
this path.

## Shutdown and failures

Shutdown disconnects the local client, finalizes runtime ownership, stops the host, clears
presentation state, and destroys the session. Renderer shutdown remains application-owned and is
performed before the native window closes. Startup is transactional: invalid content, duplicate
module contracts, scheduler cycles, incompatible saves, transport failures, or missing player
content return a typed error and do not yield a partially running session.

An error after a frame has begun authoritative or client synchronization is terminal for that
session because event/delta intake may already have advanced. `RuntimeSession` records the original
typed fault, rejects subsequent frames and commands, exposes the fault through `GameInspector`, and
still permits deterministic shutdown. Snapshot capture failures do not fault the session because
they operate on a disposable snapshot and perform no disk commit.

No globals or service locator participate in the frame path. Dependencies are constructor inputs,
registration contexts, or narrowly scoped tick contexts.
