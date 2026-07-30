# Heartstead Engine Specification v0.2

**Status:** normative architecture contract

**Scope:** engine infrastructure; gameplay/content rules are deliberately out of scope

This specification describes the intended durable engine shape. It is not an implementation-status
checklist. Read [Project status](../project_status.md) for current coverage and limitations, and use
architecture decision records for the rationale behind foundational choices.

## 0. North Star

Heartstead is a co-op voxel settlement survival game in which the settlement and its
infrastructure are the primary progression system. The engine is therefore a voxel settlement
simulation engine, not a collection of Minecraft-specific assumptions.

It must support large editable worlds, true cubic streaming, 64-bit coordinates, rich block
models, physical in-world workpiece crafting, persistent settlements, multiblock assemblies,
lazy offline processes, server-authoritative co-op, heavy modding, resource/shader packs,
administrator tooling, and long-lived saves.

## 1. Architecture law

The engine keeps these representations separate:

```text
WorldGrid       cubic chunks, block cells, terrain, ores, soil, caves, fluid cells
BlockLayer      prototypes, states, render/collision/selection/occlusion, sparse metadata
PlacementWorld  build pieces, furniture, carts, trees, loose cargo, entities
AssemblyWorld   kilns, furnaces, mills, wards, machines, multiblock validation
WorkpieceWorld  small editable local crafting grids
SimulationWorld rooms, processes, farms, fire, weather, power, wards, logistics
NetworkWorld    authoritative commands, replication, profiles, discovery, admin logs
ModWorld        prototypes, resources, shaders, scripts, conflicts, migrations
```

Terrain voxels are not workpiece voxels. World blocks are not assemblies. Render geometry is
not collision geometry. Server state is not client presentation. Vanilla content is not
hardcoded engine behavior.

## 2. Coordinates, chunks, and regions

All authoritative cell and chunk positions use signed 64-bit integer components.

```text
BlockCoord  x/y/z: i64
ChunkCoord  x/y/z: i64
LocalCoord  x/y/z: 0..31
Chunk       32 x 32 x 32 cells
Region      8 x 8 x 8 chunks (256 x 256 x 256 cells)
```

Negative coordinates must use floor division, not truncating division. Checked conversions must
reject arithmetic overflow. No subsystem may clamp world identity into 32-bit coordinates.

Rendering uses camera-relative floating coordinates. Physics uses local simulation-island
coordinates. Save and network data preserve integer anchors. Placed-object transforms that need
fractions use an integer world anchor plus a bounded local offset.

A chunk owns compact cells, block states, sparse metadata references, generation stamp, edit/save/
mesh/collision revisions, and dirty flags. It does not own entities, assemblies, rooms, player
profiles, discovery, global time, or server logs.

## 3. Rich block model

A compact block cell contains a palette/prototype reference, compact state bits, and an optional
sparse metadata handle. Block prototypes independently declare:

```text
logical occupancy
render model and render bounds
collision model and bounds
selection model and bounds
occlusion behavior
light emission and absorption
material/gameplay tags
metadata requirements
neighbor dependency radius
mesh invalidation radius
```

Render geometry may extend outside its owning cell. Collision, selection, and occlusion do not
implicitly follow render bounds. Mesh jobs request the owning chunk plus a prototype-derived
neighbor halo. Edits invalidate all affected chunks/cells according to the block model, not only
the six immediate neighbors.

Supported model families include cube, parametric slab/stair, cross-plane foliage, voxel-like
custom model, small glTF-style mesh, connected texture, overlay/protrusion, random variant, and
state-dependent model.

Block categories include solid terrain, placed voxel blocks, decorative rich blocks, interactive
blocks, metadata-backed block entities, and fluid cells. Rich blocks are not automatically
entities.

## 4. Spatial representations

- World blocks: terrain, caves, mines, soil, ore, clay, water, voxel construction, rich decoration.
- Build pieces: freely placed walls, roofs, furniture, stations, docks, bridges, and roads.
- Assemblies: logical machines composed from blocks/build pieces and validated against data.
- Workpieces: small 2D/3D editable crafting grids.
- Entities: players, creatures, animals, carts, boats, projectiles, dropped items, falling trees.
- Derived fields/graphs: rooms, paths, logistics, power, wards, heat/light, stability, discovery.

Derived caches remain rebuildable unless a versioned cache has an explicit persistence reason.

## 5. Workpieces and pattern library

A workpiece stores stable ID, prototype/material, grid dimensions/cells, optional blob and hidden
masks, planning/template masks, per-cell pattern IDs, smoothing/decor flags, measured output
metadata, owner/session state, revisions, and save/replication state.

Knapping uses a 2D logical grid, server-generated hidden flaws, client-local planning, and
server-resolved strikes. Clay forming uses a layered 3D grid, additive/removal edits, material
budget, templates, smoothing, measured structure, and server finish validation.

The shared mod-defined `PatternLibrary` supports 2D patterns, 3D negative mould patterns,
rotation/mirroring, material constraints, and output mapping. World chunks and workpiece grids
never share storage or mutation commands.

## 6. Authoritative time and lazy processes

The server owns exactly one monotonic value:

```text
world_time: u64 ticks
```

Calendar, daylight, season, weather, schedules, and process conditions derive from it. Deterministic
global conditions are functions of world time, location, seed, and world configuration.

There is no global per-process ticker. A `ProcessInstance` stores prototype, owner, start,
accrued work, last evaluation, state, inputs/outputs, claim state, condition function, and
interruption policy. Evaluation occurs on load, interaction, render proximity, relevant state
change, inspection, or save/load validation. Elapsed intervals are replayed deterministically.

Sleeping advances `world_time`; it does not special-case each process.

## 7. Fire, clay, firing, and casting engine contracts

Fire is a lazy-evaluated fuel-buffer state (`unlit`, `lit`, `embers`, `out`) with last evaluation,
weather exposure, light/warmth/repel radii, cook slots, and transfer interactions. Age 0 has no
voxel fire spread, burning buildings, smoke-fluid simulation, or survival temperature pressure.

Clay supports `green -> dry -> fired`, measured mass/structure, offline drying, firing outcome
tables, damaged material/mesh variants, and reclaimable shards/grog. Pit firing and kiln firing
are processes attached to station/assembly state.

Casting supports molten state, pour windows, held crucible state, nearby mould targeting, cooling,
single-use/reusable mould state, effects, and wear-band recycling. These are generic engine
capabilities; actual recipes and balance remain mod/game data.

## 8. Assemblies and construction ghosts

An assembly contains stable ID, root anchor, typed part list, required/optional roles, named ports,
spatial validation rules, capabilities, process slots, state machine, and save/network state.

Named ports include item/fuel/fluid/heat/smoke/air/power/ward/operator/cart/storage/mould/pour
roles. Mods define layouts, part roles, port locations, validation, processes, capacity, room/
power/heat requirements, failure states, and UI panels.

The kiln is the first proving slice: ghost blueprint, staged part placement, layout validation,
mortar drying, maiden firing, final station state, capacity, and heat tier.

## 9. Resources, materials, and shaders

Resource packs may override presentation assets—textures, models, block models, animation, audio,
fonts, icons, UI skins, material parameters, shader presets, particles, and sky/weather visuals—
without changing gameplay unless they are also gameplay mods.

Texture support includes arrays, per-face/overlay textures, deterministic random variants,
connected textures, emissive masks, seasons, biome tint, and pack overrides. Shader packs use
validated engine extension points for materials, foliage, water, ore/glow, post-processing,
fog/weather, debug overlays, and sky. Mods never receive raw Vulkan access.

## 10. Deterministic cubic world generation

```text
generate_chunk(seed, chunk_coord, world_config, prototype_db)
```

World generation supports biomes/sub-biomes, caves, deep strata, deposits, future-tool and
mastery-return layers, fluids, ruins, resource sites, normal/rich blocks, block entities, surface
objects, and large hybrid/static resources. Output is deterministic and independent per cubic
chunk with explicit cross-chunk feature ownership.

## 11. Save system

World saves store seed/config/time, active mods/versions/hashes, palette/prototype mappings,
3D regions/chunks, edits, placed objects, assemblies, entities, processes, player-profile
references, log index references, and migration history.

Chunk records store signed 64-bit coordinates, generation stamp/checksum, palette, cell/state data,
sparse metadata, and revisions. Region files group `8 x 8 x 8` chunks. Stable string IDs and
migrations protect content identity; compact runtime palette numbers are translated through a
persisted palette table.

If a prototype is missing, the save retains a `MissingPrototypeObject` containing original ID,
opaque blob, position/owner, and warning state. Missing content is never silently deleted or
reinterpreted.

## 12. Server-side player profiles and map discovery

Each stable player UUID owns a server-side profile containing display-name history, roles,
spawn/bed, layered map discovery, waypoints/markers, handbook/progression flags, portable world
settings, and optional character/cosmetic data.

Discovery is stored in compact region bitsets keyed by layer and signed region coordinate. Layers
include surface, underground/caves, and optional ward/admin layers. The server updates discovery,
sends incremental revisions, persists periodically/on leave, and restores it on a new client PC.

```text
world/player_profiles/<player_uuid>/profile.dat
world/player_profiles/<player_uuid>/map_discovery.dat
```

## 13. Administrator logs

The authoritative server records join/leave, chat, commands, moderation, permissions, save/load,
mod mismatch, crash/recovery, and optional edit/inventory audit events. Every entry carries a UTC
ISO-8601 real timestamp, `world_time`, derived world calendar text, session, event type, and
relevant stable player identity.

Logs are append-only, rotated by size/day, compressed after rotation, indexed for search, and
independent of clients. Chat is written before or atomically with broadcast.

```text
server/logs/current.log
server/logs/chat/current.log
server/logs/audit/current.log
server/logs/rotated/<timestamp>.<category>.<sequence>.log.hsz
server/logs/rotated/index.log
```

The current implementation uses the engine-owned, lossless `HSLZ1` archive codec; `.hsz` is not a
zstd stream. Queries scan structurally valid archive names in timestamp/numeric-sequence order and
then the current file. Reads, expanded archives, result counts, directory scans, and archive counts
are bounded. Log roots, category directories, current files, archive files, temporary files, and
the rotation index reject symbolic links. Rotation publishes a complete archive before removing the
current file, detects concurrent source growth, and removes a newly published archive if the current
file cannot be removed so a query cannot return duplicate history. The index is atomically replaced
after a successful rotation; archives remain the authoritative query source if index publication
fails. Authoritative-server startup creates and opens every category log and the index, then validates
the bounded archive catalog before the transport host starts accepting clients.

## 14. Server authority and replication

Clients send intents; the server validates permissions, reach/ownership, prototypes, state, and
preconditions before mutation. Commands cover block/build/workpiece/process/fire/assembly/cargo/
inventory/sleep/chat/discovery changes.

Replication has a server-owned stream sequence independent of each client's command sequence.
Interest filtering applies to the actual event and state payload, not merely to the decision to
send a whole batch. Hidden subjects, private annotations, and reserved IDs must not leak.

Join validates authentication and content compatibility, loads/creates the profile, sends world
config/time/profile/discovery, then streams nearby chunks and logs the join. Leave flushes profile/
discovery, logs the event, and clears session state.

## 15. Modding

Vanilla content lives in `mods/base` and uses the public mod pipeline. Stages are settings,
prototype/data, data updates, final fixes, assets/resources, shader validation, save migration,
runtime server scripts, and runtime client scripts.

Mods can define blocks/models/states, items/tools/materials/recipes/processes/workpieces/patterns,
assemblies/build pieces, entities/animals/crops/biomes/features, rooms/networks/wards/fire/map
layers, UI, shaders/materials, and admin commands.

Data is preferred over per-tick script. The resolved prototype database records creator, every
patch and stage, final per-field origin, conflicts, and warnings. A prototype inspector exposes
that trace.

## 16. Rooms, logistics, and physical resources

Rooms derive from terrain, blocks, build pieces, portals, and assemblies and expose readable
descriptors. Inputs include enclosure, roof/walls/doors, terrain/underground contact, light/heat/
smoke/ventilation, storage/cart access, power, wards, and later cleanliness/dampness.

Cargo distinguishes inventory items, bulk cargo, haulable/vehicle/hazard objects, and molten or
unstable states. It records mass, volume, stackability, container constraints, stability/spoilage,
hazards, transport modes, and load/unload state.

Roads, paths, bridges, docks, cart access, animal routes, storage access, warehouses, outposts,
and warded roads form spatial networks. They may affect movement speed, cart throughput, animal
stamina, pathfinding reliability, heavy transport, safety, corpse recovery, weather resistance,
and creature avoidance; they are not merely cosmetic terrain tags.

Physics is limited to players/creatures, vehicles, boats, falling trees/logs, physical cargo, and
bounded debris. Terrain blocks, leaves, process clocks, rooms, and settlement simulation are not
individual physics bodies. Physics uses local origins.

Felled trees use an explicit resource lifecycle:

```text
standing static resource -> cutting state -> falling compound body
                         -> settled sleeping/frozen object -> processed cargo/resources
```

## 17. Rendering and debug infrastructure

The renderer handles chunk meshes, rich/out-of-cell models, foliage, protrusions, emissives,
firelight, particles, placed objects, workpieces, entities, and debug draw. Camera-relative
transforms are derived from integer anchors.

Mandatory overlays include chunk boundaries/coordinates, 64-bit position, render/collision
bounds, mesh dirtiness, workpiece grids, assembly parts/ports, rooms, light/fire/repel radii,
process state, map masks, network interest, save IDs, and server log tail.

## 18. Administrator and server management

The engine provides a server console plus join/leave/chat and audit logs, save/backup commands,
profile export/import, discovery inspect/reset, permissions, ban/kick lists, mod-list verification,
world-time inspection, teleport/locate, chunk inspection, and prototype inspection.

Log queries support real time, world time, player UUID, historical display name, event type,
message content, and server session. Administrator actions use the same permission, logging,
stable-ID, and server-authoritative command boundaries as player actions.

## 19. Repository boundaries

The repository is organized by ownership rather than by one vertical-slice milestone:

```text
engine/     reusable infrastructure and public engine contracts
 game/      Heartstead runtime composition and game-owned systems
 mods/base/ base content loaded through the public mod/prototype pipeline
 apps/      executable server, client, development, smoke, and benchmark compositions
 samples/   focused demonstrations of one or a few boundaries
 tools/     cookers, validators, inspectors, and diagnostics
 tests/     executable contracts, fixtures, integration, and regression coverage
 docs/      maintained contracts, status, workflows, and benchmark methodology
```

`engine/` must not depend on `game/` or hard-code `mods/base` content identity. Applications choose
roles and backends but do not redefine authority. Tools use public formats and inspection APIs
instead of reaching into private implementation storage. Base content must remain replaceable and
patchable through the same mod lifecycle exposed to other mods.

Subsystem directories may evolve. Directory names are not an architecture contract unless a public
asset, save, or mod format explicitly relies on them.

## 20. Change requirements

Changes to engine behavior follow these rules:

1. Change the architecture contract, implementation, tests, and maintained documentation together.
2. Route authoritative mutation through commands, validation, and transactions.
3. Give asynchronous workers immutable bounded snapshots and reject stale results by revision or
   generation on the owner thread.
4. Put explicit limits on queues, allocations, payloads, retries, scans, per-frame work, and
   persistent records; make overflow visible.
5. Version save, network, replay, cooked-asset, and script/API formats before incompatible changes.
6. Preserve stable string identity and migration/placeholder behavior across compact runtime
   mappings.
7. Keep headless, local server/client, remote client, and dedicated-server roles first-class where
   the relevant subsystem applies.
8. Keep backend-native types private behind engine-owned interfaces.
9. Put reusable mechanisms in `engine/`, Heartstead orchestration in `game/`, and authored meaning
   in mods.
10. Verify changes with focused tests plus the appropriate compiler, sanitizer, native, socket, or
    benchmark path described in [Testing](../dev/testing.md).

Completed milestone reports are evidence in version history, not permanent sources of current
truth. Maintained pages describe durable contracts, current status, operations, and reproducible
measurement.

## 21. Non-negotiable constraints

1. All world cell/chunk coordinates are signed 64-bit integers.
2. Chunks and regions are cubic, not vertical columns.
3. Render, collision, selection, and occlusion representations are separate.
4. Render geometry may extend outside its owning cell.
5. Vanilla loads through the same mod/prototype pipeline.
6. Workpiece grids are separate from world chunks.
7. Long-running processes derive lazily from one server `world_time`.
8. Discovery lives in the server-side player profile.
9. Join/leave/chat logs carry real UTC and world timestamps on the server.
10. Multiblock stations are data-defined assemblies, not hardcoded scans.
11. Saves use stable string IDs, persisted mappings, placeholders, and migrations.
12. Clients send intents; the server owns truth and filters private replication payloads.
13. Debug overlays are engine infrastructure.

## 22. Target engine shape

```text
Heartstead Engine
 |- Core: IDs, logs, asserts, memory/jobs/math, i64 coordinates
 |- Platform: windows/input/filesystem
 |- World: cubic chunks, rich blocks/models, worldgen, regions, discovery
 |- Rendering: Vulkan, meshing, rich/out-of-cell models, materials, overlays
 |- Simulation: world time, lazy processes, fire, rooms, logistics, power/wards
 |- Crafting: workpieces, patterns, knapping, clay, drying/firing, casting
 |- Settlement: build pieces, assemblies, ghosts, stations, physical cargo
 |- Network/Server: authority, replication, profiles, discovery sync, logs/audit
 |- Modding: prototypes, patches/conflicts, resources/shaders, scripts, migrations
 `- Tools: block/chunk/save/prototype/map/log/replay inspectors
```

The implementation may realize these capabilities incrementally, but a temporary executable or
milestone must not weaken the contracts above. Current implementation coverage and known gaps are
tracked in [Project status](../project_status.md); operational verification lives in
[Testing](../dev/testing.md).
