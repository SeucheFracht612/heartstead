# Voxel Palette

The voxel palette maps mod-defined voxel prototypes to compact terrain cell ids.

Implemented foundation:

- `VoxelDefinition`
  - content voxel type id
  - source prototype id
  - display name
  - terrain material token
  - mining tool token
  - optional tags
  - optional block-model prototype, logical occupancy, collision/selection/occlusion bounds,
    occlusion behavior, light emission/absorption, and metadata requirement
  - explicit missing-prototype marker for a persisted mapping whose definition is unavailable

- `VoxelPalette`
  - reserves type `0` for air
  - assigns content voxel types starting at `1`
  - looks up definitions by type or prototype id
  - creates `VoxelCell` values from prototype ids
  - owns validated block-model definitions and exposes mesh/neighbor invalidation radii
  - compares old/new cells by collision geometry, light emission/absorption, and fluid-solver
    occupancy/state so edit scheduling can invalidate only affected derived consumers
  - exports a `VoxelPaletteManifest` of compact type-to-prototype assignments

- aggregate content validation
  - builds the palette from loaded `voxel` prototypes
  - exposes the palette to tools and game-runtime startup reports

- save/recovery construction
  - text and binary snapshots persist the palette manifest
  - the persisted-manifest builder preserves every saved numeric assignment, materializes a visible
    missing-block definition for an unavailable voxel prototype, and can append new prototypes
    above the highest persisted type without renumbering saved entries

Chunks intentionally store compact numeric `VoxelCell` values. Mods and gameplay systems
should speak stable prototype ids such as `base:voxels/clay`, then resolve through the
palette at command/worldgen boundaries. This keeps streamed terrain storage small without
turning chunk cells into the only source of voxel meaning.

Type `0` is permanently reserved for air. Content voxel ids are deterministic for a given
loaded prototype set because the palette sorts voxel prototype ids before assigning types.
Save metadata already records normalized per-mod prototype fingerprints and the snapshot records
the exact numeric mapping. The current normal runtime policy still requires matching active mod
fingerprints; the persisted-manifest builder is a recovery/migration primitive, not permission to
silently accept semantically changed voxel definitions.
