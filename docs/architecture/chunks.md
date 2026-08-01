# Chunk Database

World voxels are stored in streamed chunks. The chunk database is the engine-owned
container for those chunks and their edit records. Chunk cells store compact numeric
types; mod-facing voxel meaning is resolved through the voxel palette.

Implemented foundation:

- `VoxelChunk`
  - fixed-size terrain voxel storage
  - each `VoxelCell` preserves compact type, light, state bits, and metadata handle
  - cubic chunk coordinates use signed 64-bit `x/y/z` components for near-unbounded world height
    and distance
  - dirty flags for mesh, collision, lighting, save, and replication
  - monotonic content revision starting at one, advanced only when stored cells actually change
    (plus generated-cell replacement), so renderer work can prove which content it represents
  - compact owner-thread stage ledger for content, lighting, mesh, collision, persistence, and
    replication; each derived stage keeps an independent request epoch, resident request epoch,
    output revision, lifecycle state, and stale/cancelled totals
  - generation-counted stage tickets that let asynchronous or re-entrant work prove both the exact
    chunk residency and the exact stage request it was built to satisfy
  - a process-unique, never-reused load generation assigned only while resident in a
    `ChunkDatabase`; coordinate plus generation forms `ChunkIdentity`

- `VoxelPalette`
  - maps stable voxel prototype ids to compact content voxel type ids
  - reserves type `0` for air
  - keeps prototype meaning out of chunk cell storage

- `ChunkDatabase`
  - create/find chunk records
  - expose read-only chunk snapshots for inspection, tooling, and simulation LOD derivation
  - insert generated chunks without creating edit records
  - read and write voxel cells
  - append explicit voxel edit records
  - auto-create edited chunks
  - erase chunk records when the streaming layer unloads them
  - expose sorted loaded identities for initial renderer synchronization without scanning voxels
  - mark edited chunks dirty for mesh, collision, lighting, save, and replication
  - apply saved chunk edit deltas without re-marking loaded chunks dirty for save or replication
    while preserving those deltas for later snapshot export
  - mark existing face-neighbor chunks dirty for rebuild when boundary voxels change
  - palette-aware edits additionally mark every resident chunk reached by the old/new block
    model's declared mesh invalidation radius
  - expose dirty/edit statistics
  - aggregate requested, running, ready, resident, stale, and cancelled lifecycle counts per stage;
    runtime inspection exposes the same counters without scanning worker-owned state
  - optionally emit dirty regions for mesh, collision, and lighting rebuild queues
  - preserves signed 64-bit chunk coordinates in dirty-region rebuild queues without clamping

- `ChunkEditDeltaTextCodec`
  - writes version 2 text deltas with complete previous/next type, light, state-bit, and metadata
    cells; accepts version 1 by supplying zero state/metadata defaults
  - preserves signed 64-bit chunk coordinates in saved edit payloads
  - validates saved chunk coordinates against encoded payload coordinates
  - shared by world snapshot export/import and save snapshot validation
  - rejects discontinuous multi-edit chains for one voxel when a delta is applied by
    `ChunkDatabase`

- `ChunkStreamer`
  - loads chunk coordinates into `WorldState` on demand
  - generates baseline terrain through the deterministic terrain generator
  - can overlay one saved per-chunk edit delta through an abstract delta-source interface
  - provides a `FileSaveDatabase` adapter that maps a missing per-chunk delta to "no saved edits"
    without exposing save-directory layout to world streaming callers
  - reports whether a request found an already-loaded chunk, generated a fresh baseline chunk, or
    generated a baseline chunk with saved edits applied
  - returns the resident `ChunkIdentity` in every successful load report and the exact evicted
    identities in eviction reports, preventing stale work from removing a reloaded generation
  - applies saved deltas through the load-specific chunk path so streamed loads do not create false
    save or replication dirtiness
  - stages generated data plus saved edits transactionally, leaving no loaded chunk when validation
    or delta application fails
  - validates every saved local coordinate and edit chain before mutation, verifies the first
    `previous` cell against the generated baseline, and restores canonical edit history without
    duplicating records across reloads
  - plans viewer-interest load requests from a cylindrical horizontal radius plus an independent
    vertical radius without mutating world state; diagonal columns outside the circle are not
    loaded merely because they fit a square bounding box
  - uses separate, larger retain radii as streaming hysteresis so viewer movement near an interest
    boundary does not immediately unload and reload the same chunks
  - bounds both load radii and uses overflow-safe distance/range arithmetic at signed coordinate
    limits
  - reports clean chunks outside the retain cylinder as evictable while pinning save-dirty or
    replication-dirty chunks so terrain edits are not discarded before persistence/replication
  - executes eviction requests by removing only clean loaded chunks, reporting missing chunks and
    retained dirty chunks separately
  - flushes requested save-dirty chunk edit deltas through an abstract sink, with a
    `FileSaveDatabase` sink adapter for streamed persistence
  - clears only the save-dirty flag after a successful chunk-delta write; replication-dirty chunks
    remain pinned until the replication side has handled them
  - flushes requested replication-dirty chunk edit deltas through an abstract replication sink
  - clears only the replication-dirty flag after a successful replication handoff, so eviction can
    proceed only after both persistence and replication have acknowledged the terrain edits
  - captures immutable save/replication payloads plus stage tickets before invoking a sink, then
    re-resolves the chunk identity and request epoch before acknowledging; an edit, unload, or reload
    during a sink call is reported as stale and cannot clear newer dirty state
  - can run one viewer-interest maintenance step that plans chunk interest, optionally flushes
    pinned dirty chunks through save and replication sinks, and then evicts every now-clean
    candidate while reporting chunks that still cannot be unloaded
  - can also run a loaded maintenance step that satisfies viewer-interest load requests through
    deterministic generation plus optional saved edit deltas before the flush/evict pass

- `ChunkMesher`
  - provides a reference surface extractor and a production greedy extractor
  - uses immutable center-plus-halo snapshots for cross-chunk visibility and rich block-model
    dependencies
  - emits material/render-phase sections plus rich-model instances separately from indexed terrain
  - preserves voxel type, light, and state bits per vertex

Neighbor invalidation marks rebuild state only. It must not mutate neighboring voxel
data. Save and replication dirtiness remain attached to chunks whose stored data changed.
Generated chunks and loaded saved edit deltas only mark mesh, collision, and lighting
rebuild state. They do not mark save or replication dirty until a player/world operation
changes stored voxel data after load.

Stage transitions are owner-thread operations. `requested -> running -> ready -> resident` is the
successful path. A new invalidation always advances that stage's request epoch while retaining the
older resident product until replacement. Obsolete results increment stale/cancelled telemetry but
cannot change the state of a newer request. Worker payloads contain tickets and immutable snapshots;
workers never receive a mutable `VoxelChunk` or `ChunkDatabase`.

Current extension areas:

- budgeted/asynchronous streaming jobs for large load and eviction waves
- further mesh compression, simulation/render LOD, and rich-model batching optimization

Collision cooking and voxel-light propagation are maintained as separate systems; see
[physics](physics.md) and [voxel lighting](voxel_lighting.md).
