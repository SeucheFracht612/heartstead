# Chunk Meshing

Chunk meshing converts terrain voxels into a renderer-neutral surface mesh. It does not
own voxel storage, renderer buffers, Vulkan handles, materials, or gameplay meaning.

Implemented foundation:

- `ChunkMeshVertex`
  - local position
  - face normal
  - UVs
  - source voxel type
  - source voxel light
  - source voxel state bits

- `ChunkMesh`
  - source chunk coordinate
  - local bounds
  - vertices
  - uint32 indices
  - material/render-phase index sections
  - separate rich block-model instances with owning cell, render bounds, state bits, and metadata
  - face count
  - required/provided halo radii
  - validation for counts, bounds, finite vertices, index ranges, exact section coverage, rich
    instances, and sufficient halo input

- `ChunkMesher::build_surface_mesh`
  - emits faces for solid voxel sides exposed to air or chunk boundaries
  - hides internal faces between adjacent solid voxels
  - returns an empty valid mesh for fully air chunks
  - has a worker-safe overload that consumes only `ChunkNeighborhoodSnapshot` and
    `BlockRenderTableSnapshot`

- `GreedyChunkMesher::build`
  - is the default mode for `ChunkMeshScheduler` and `ChunkRenderSystem`
  - merges compatible exposed terrain faces while retaining material/render phase, voxel type,
    light, state bits, and block flags as merge boundaries
  - does not split compatible geometry merely because neighboring cells select different authored
    texture variants; the terrain fragment path recovers the owning local cell within the merged
    quad and selects the stable per-face variant
  - emits the same validated `ChunkMesh` contract as the reference extractor

- Immutable worker input
  - `ChunkNeighborhoodSnapshot` stores contiguous center-plus-halo cells; ordinary cube visibility
    uses a 34 x 34 x 34 snapshot for a 32-cubed chunk
  - dependency records cover both present and absent neighboring chunks and retain load generation
    plus content revision for every required chunk
  - `BlockRenderTableSnapshot` resolves voxel definitions and block models into immutable,
    contiguous lookup data before a worker enters the cell loop
  - snapshot buffers are reused by `ChunkMeshScheduler` and snapshot copying is bounded per frame

- Asynchronous result contract
  - every request and result carries `ChunkIdentity`, center revision, dependency revisions,
    block-render-table revision, and scheduling priority
  - a fixed worker pool coalesces duplicate chunk work, limits concurrency, and observes
    cancellation before meshing begins
  - completed meshes cross a mutex-protected typed mailbox and are drained only on the renderer
    owner thread
  - center edits, neighbor edits, unload/reload generation changes, and render-table changes reject
    old results before upload; the newest revision is requeued
  - dirty state is cleared only after a validated replacement is resident

This remains a CPU-side extraction contract. Workers must never retain or query mutable world or
chunk objects. Further mesh compression, LOD, and rich-model batching can evolve without changing
chunk storage into renderer data or weakening the snapshot boundary.
