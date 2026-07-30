# World Model Architecture

The engine keeps world-scale terrain voxels separate from local crafting workpieces.

Implemented foundation:

- `engine/world/voxels/VoxelChunk`
  - fixed-size terrain chunk storage
  - voxel cells for terrain/world mass ids
  - dirty flags for mesh, collision, lighting, save, and replication

- `engine/world/voxels/VoxelPalette`
  - stable mod voxel prototype ids mapped to compact chunk cell type ids
  - type `0` reserved for air

- `engine/world/chunks/ChunkDatabase`
  - chunk lookup and creation
  - generated chunk insertion
  - voxel edit records
  - boundary neighbor invalidation
  - dirty statistics

- `engine/world/meshing/ChunkMesher`
  - reference and greedy renderer-neutral extraction from immutable chunk-neighborhood snapshots
  - cross-chunk occlusion and prototype-declared neighbor halos
  - material/render-phase mesh sections and separate rich-model instances
  - mesh vertices preserve voxel type, light, and state bits

- `engine/world/worldgen/DeterministicTerrainGenerator`
  - seed/region/palette-driven baseline chunk generation
  - generated chunks are rebuild-dirty but not save/replication dirty

- `engine/world/streaming/ChunkStreamer`
  - composes baseline generation with optional saved per-chunk edit deltas
  - loads chunks into `WorldState` without making world streaming know save-database file layout
  - reports generated, generated-with-saved-delta, and already-loaded outcomes for tooling/tests
  - plans viewer-interest load requests and eviction candidates while pinning dirty chunks until
    persistence/replication has handled them
  - flushes requested save-dirty chunk edit deltas through an abstract sink and clears only the
    save-dirty flag after a successful write
  - flushes requested replication-dirty chunk edit deltas through an abstract replication sink and
    clears only the replication-dirty flag after a successful handoff
  - can run a viewer-interest maintenance step that flushes pinned dirty chunks when sinks are
    provided, then evicts clean candidates while retaining any chunk still dirty for save or
    replication
  - can satisfy viewer-interest load requests through deterministic generation plus optional saved
    edit deltas before running the same flush/evict maintenance pass
  - unloads only clean chunks and reports dirty or missing eviction requests without mutating save
    metadata, edit logs, or other world representations

- `engine/world/regions/RegionGraph`
  - broad authored/generated world structure
  - region descriptors for age, biome cluster, resource rules, gradients, layers, and ecology
  - region connections for route/outpost/worldgen context

- `engine/workpieces/WorkpieceGrid`
  - compact local editable grid
  - add/remove/set cell operations
  - operation history suitable for authoritative server validation
  - template matching and grid text codec

- `engine/build/BuildPieceRecord`
  - placed settlement construction with sockets, room tags, and network ports

- `engine/assemblies/AssemblyRecord`
  - logical multiblock structure state validated against named requirements

- `engine/world/WorldState`
  - authoritative runtime container
  - separate stores for regions, chunks, build objects, entities, cargo, inventories,
    workpieces, physical resources, assemblies, processes, fires, derived rooms, networks, missing
    prototype placeholders, and mod state
  - shared save id and runtime handle allocation
  - shared dirty-region tracker

- `engine/world/WorldSnapshotBridge`
  - export from runtime world state to typed save snapshot sections
  - import from typed save snapshot sections back into runtime world state

- `engine/world/ReplicationInterest`
  - derives per-client replication interest from world simulation subjects and viewer positions
  - emits inspectable derivation reports so tools can see saved-subject visibility, LOD
    exclusions, and skipped non-saved derived subjects before the policy reaches networking
  - can apply the derived policy to a live host session while returning the same report for
    inspection
  - keeps network relevance connected to world state without making the net layer understand
    build pieces, entities, cargo, workpieces, assemblies, processes, networks, or terrain chunks

- `engine/world/ReplicationDelta`
  - plans authoritative replication deltas from committed event batches and `WorldState`
  - keeps global events separate from saved-object subject events
  - classifies saved subjects by the concrete world stores that own them instead of introducing a
    universal replicated object store
  - materializes typed delta sections using existing build-piece, entity-save, cargo, inventory,
    workpiece, assembly, and process record shapes
  - converts authoritative host tick command reports into per-command typed delta snapshots while
    leaving failed, read-only, and eventless commands explicit
  - applies typed delta snapshots back into `WorldState` through per-representation upserts while
    preserving client-local runtime/session ids for persistent entities
  - drains client replication intake through a world-layer adapter that applies only matched typed
    deltas and reports subject-event batches still waiting for authoritative records
  - owns the typed-delta replication payload contract over transport envelopes so networking stays
    byte/session-oriented
  - delivers complete typed delta snapshots to relevant host clients through an inspectable
    world-layer report while skipping partial deltas that require snapshot/resync fallback
  - drains queued typed delta envelopes from client protocol sessions without moving world-store
    decoding into networking
  - encodes/decodes bounded versioned binary delta snapshots for live transport
  - retains deterministic text delta snapshots for tools and compatibility fixtures
  - reports unresolved subject ids so the server/client can choose snapshot/resync fallback before
    applying invalid assumptions

- `engine/debug/Inspector`
  - structured `WorldState` inspection with save metadata, allocator identity, dirty-region
    count, and per-representation database counts

These modules intentionally do not share cell types or storage. Regions describe broad
world context. The voxel palette bridges mod voxel prototypes to compact terrain cell ids.
Terrain chunks are for streamed world mass. Workpiece grids are for tactile local crafting
objects.

`heartstead_world_inspector` can print this live world-state inspection directly, or import
a text save snapshot through `WorldSnapshotBridge` and inspect the resulting runtime world.

`RoomGraph` is held by `WorldState` as rebuildable derived state. It is counted and
inspectable at runtime, but it is not exported as authoritative save data.

Future world model work:

1. richer biome/region transitions and runtime placement of generated feature records
2. further mesh compression, LOD, and rich-model batching optimization
3. scheduling save/replication flushes from the streaming interest plan automatically
4. richer derived worlds for roads, power, smoke, heat, and navigation

Save snapshots already model chunk edits as their own section. Derived worlds should
remain rebuildable and should not be saved as authoritative data unless a specific cache
version is justified.
