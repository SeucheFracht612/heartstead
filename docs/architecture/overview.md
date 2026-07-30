# Heartstead architecture overview

Heartstead separates durable simulation state from presentation and keeps game meaning outside the
engine. The design is aimed at a long-lived, heavily modded, server-authoritative voxel settlement
simulation rather than a single hard-coded gameplay slice.

For implementation status, read [Project status](../project_status.md). For normative long-term
constraints, read the [Engine specification](engine_spec.md).

## System boundary

```text
mods and resource packs
        |
        v
content discovery -> validation -> resolved prototypes/assets/scripts
        |                              |
        v                              v
game systems -> commands -> authoritative WorldState -> events/snapshots
                         |                    |
                         |                    +-> saves, logs, replay, inspection
                         v
                  replication/transport
                         |
                         v
client state -> presentation extraction -> renderer/audio/UI/particles
```

The engine supplies reusable storage, validation, scheduling, rendering, transport, persistence,
and tool boundaries. The game layer composes those boundaries and assigns Heartstead-specific
meaning. `mods/base` provides base content through the same public pipeline available to other mods.

## Core representation split

The engine keeps distinct representations for distinct jobs:

```text
WorldGrid       cubic chunks, terrain cells, fluids, lighting, generation
BlockLayer      prototypes, state, bounds, occlusion, sparse metadata
PlacementWorld  entities, build pieces, furniture, carts, loose cargo
AssemblyWorld   data-defined machines, ports, validation, process slots
WorkpieceWorld  small editable crafting grids independent of world chunks
SimulationWorld time, processes, rooms, networks, farms, power, wards
NetworkWorld    commands, sessions, replication, profiles, discovery, logs
ModWorld        prototypes, assets, scripts, patches, conflicts, migrations
```

This separation prevents convenient implementation details from becoming permanent game rules.
Terrain voxels are not workpiece voxels; render meshes are not collision; session IDs are not save
identity; client presentation is not authoritative simulation.

## Ownership and authority

The authoritative server owns meaningful state, stable IDs, world time, command validation,
transactions, persistence dirtiness, and replication events. Clients send intents and may predict
presentation-sensitive movement, but corrections always converge on server state.

A local game still follows this boundary: its server and client happen to share a process and use
an in-memory transport. A remote client and dedicated server use the socket-backed transport while
preserving the same command and replication semantics. See [Runtime composition](runtime_composition.md)
and [Networking](networking.md).

## Data identity

Three identity domains must not be conflated:

- **Persistent identity:** stable string prototype IDs, save IDs, player UUIDs, and integer world
  coordinates survive processes and machines.
- **Runtime identity:** handles, palette indices, entity slots, revisions, and job generations are
  valid only inside their declared owner/lifetime.
- **Presentation identity:** renderer, audio, UI, and particle handles may be rebuilt without
  changing authoritative state.

Network `NetId` values and endpoints are session-local. GPU handles never enter saves or gameplay
code. Missing prototypes preserve their original stable identity and opaque data until a compatible
mod or migration can resolve them.

## Authoritative mutation flow

Meaningful mutation follows one path:

```text
input or script event
    -> command envelope
    -> permission/precondition validation
    -> transaction against authoritative stores
    -> commit or rollback
    -> deterministic events and dirty revisions
    -> persistence and recipient-filtered replication
```

Scripts emit bounded data-only events and cannot bypass commands. Tools that mutate state should
use the same validation and transaction boundaries rather than editing internal containers.

## Snapshots and asynchronous work

Worker threads operate on immutable, bounded snapshots. They do not retain live `WorldState`,
chunk, prototype-registry, renderer, or backend objects. Results carry the revisions and generations
needed for the owner thread to reject stale work.

This applies to chunk meshing, save snapshots, replication materialization, asset cooking, and other
expensive work. Queues, memory, payloads, retries, scans, and per-frame work have explicit budgets;
overflow is visible and handled rather than silently growing.

## World and coordinates

Authoritative block and chunk coordinates use signed 64-bit integers. Chunks are cubic, not vertical
columns. Negative-coordinate conversion uses floor division. Rendering is camera-relative and
physics uses bounded local islands, while saves and networking preserve integer anchors.

The world owns compact cell/state storage and revisions. Entities, assemblies, profiles, logs,
rooms, and other systems live in their own stores or derived caches. See [World model](world_model.md),
[Chunks](chunks.md), and [World state](world_state.md).

## Content and modding

Content passes through discovery, dependency/order resolution, lifecycle stages, schema validation,
semantic validation, deterministic patching, provenance tracking, and compatibility fingerprints.
The resolved prototype database is immutable to consumers for a generation and records where each
value came from.

Presentation assets and resource-pack overrides use logical IDs and cooked payloads. Gameplay mods
may define data and sandboxed scripts, but do not receive raw filesystem, network, process, Vulkan,
or native engine access. See [Modding](modding.md), [Assets](assets.md),
[Resource packs](resource_packs.md), and [Scripting](scripting.md).

## Persistence

Save code snapshots authoritative state into versioned, bounded records. Stable mappings and
migrations protect long-lived identity. Text codecs exist for fixtures and inspection; binary codecs
serve live and persistent paths where appropriate. Save publication is transactional and readers
validate sizes, versions, paths, checksums, and references before accepting data.

The local development runtime owns a save lifecycle. The current standalone dedicated executable
does not yet own one. See [Save format](save_format.md), [Save database](save_database.md), and
[Runtime composition](runtime_composition.md).

## Presentation

Renderer, audio, UI, particles, and animation consume extracted presentation data. They never own
authoritative world truth. The renderer exposes backend-neutral resources and commands; the Vulkan
backend owns all Vulkan handles and submission details, while the headless backend validates the
same contracts without claiming GPU work.

See [Rendering](rendering.md), [Animation](animation.md), [Audio](audio.md),
[Particles](particles.md), and [Game UI](game_ui.md).

## Repository boundaries

```text
engine/     reusable infrastructure; no Heartstead gameplay meaning
 game/      runtime composition and Heartstead-owned systems
 mods/base/ base data, scripts, and presentation content
 apps/      executable compositions
 samples/   focused boundary demonstrations
 tools/     cookers, validators, and inspectors
 tests/     executable contracts and regressions
 docs/      maintained contracts and contributor workflows
```

Dependencies should point inward toward stable abstractions. Gameplay code may use engine
interfaces; engine code must not depend on `game/` or `mods/base`. Backends remain private behind
engine-owned contracts.

## Where to read next

The [documentation index](../README.md) groups every maintained subsystem page. Start with the
specific owner of a behavior instead of treating this overview as an implementation inventory.
