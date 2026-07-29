# World Generation

World generation creates baseline terrain chunks from seed, region descriptors, and the
voxel palette. It is separate from player edit storage.

Implemented foundation:

- `TerrainGenerationConfig`
  - world seed
  - region id
  - signed 64-bit base surface height
  - deterministic surface variation
  - optional cave generation with per-mille frequency and minimum depth
  - per-mille resource/feature frequency
  - optional sea level and ocean fluid prototype

- `DeterministicTerrainGenerator`
  - resolves the requested region from `RegionGraph`
  - selects a terrain voxel through the region's resource rules and `VoxelPalette`
  - creates a heightfield `VoxelChunk` deterministically from seed and chunk coordinate
  - fills exposed cells at or below the configured sea level with renewable ocean source cells
  - can carve deterministic caves and replace cells with depth/placement-qualified voxel resource
    rules
  - `generate_chunk_with_features` also emits typed rich-block, block-entity, surface-object,
    large-static-object, and resource-site placement records for external rules; it does not insert
    those records into runtime stores itself
  - uses checked chunk/local-to-block conversion and rejects chunks whose cell extent cannot be
    represented by signed 64-bit `BlockCoord`
  - marks generated chunks dirty for mesh, collision, and lighting rebuilds
  - does not mark generated chunks dirty for save or replication

- `ChunkDatabase::insert_generated`
  - inserts generated chunks without creating voxel edit records
  - rejects overwriting an existing chunk
  - can emit rebuild dirty regions for generated chunks

This preserves the save model boundary:

```text
generated terrain = reproducible from seed/mod content/region descriptors
player edits      = explicit chunk edit records
```

The Foundation Slice uses a fixed voxel-native test layout rather than the general terrain
generator. New Foundation worlds record `engine/scenario.id`,
`engine/foundation.layout_version`, and the fixed Foundation seed. Loading validates the recorded
layout version before rebuilding the baseline and applying chunk-edit deltas, so deltas from an
incompatible layout fail clearly instead of being applied to unrelated terrain.

The surface is still one simple region-selected heightfield even though cave, voxel-deposit, and
typed external-feature foundations exist. Future work should add biome/region transitions, richer
strata and feature placement ownership while keeping the same boundary: worldgen produces baseline
chunks, and long-term saves persist deltas plus enough version/prototype information to migrate
them.
