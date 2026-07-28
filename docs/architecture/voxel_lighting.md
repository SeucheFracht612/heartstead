# Voxel lighting

Voxel lighting is authoritative derived world state. It is rebuilt from resident voxel types and
the immutable voxel palette; clients and save files never choose light emitted by a placed block.

## Stored field

`VoxelCell::light` remains one byte so existing chunk snapshots, edit codecs, and terrain vertices
stay compatible. The solver keeps sunlight and block light in separate scratch fields and writes
their maximum to the stored byte.

- light range: `0..255`
- ordinary six-neighbor attenuation: at least `16` per cell
- direct downward sunlight: no loss through air
- prototype absorption applies to direct and propagated light
- absorption `255` is opaque
- prototype `light_emission` seeds block light
- unknown voxel types fail dark and opaque

The separate scratch fields are important during removal: relighting starts from zero and rebuilds
both source classes, so it cannot leave an orphan value whose source is no longer present.

## Snapshot, solve, and apply

The solver consumes a `VoxelLightSnapshot` containing sorted chunk identities, content revisions,
and copied cells, plus a compact `VoxelLightBlockTable`. It never reads the live world while
propagating.

Sunlight seeding walks each contiguous loaded vertical chunk stack from top to bottom. The upper
boundary of each loaded stack is treated as sky; a missing chunk elsewhere is a closed propagation
boundary, not implicit air. A stable highest-light-first queue performs the six-neighbor flood fill
for sunlight and block light. Stable chunk/local address ordering makes results independent of
chunk insertion order.

The result contains complete, revision-tagged chunk patches. The owner thread validates every
patch before applying any of them. A generation or revision mismatch rejects the result as stale.

Applying derived light:

- advances the chunk content revision only when bytes changed;
- marks mesh and replication dirty;
- does not mark collision or save dirty;
- does not add records to the player edit log;
- clears the lighting-dirty flag after a matching patch is installed;
- emits a `chunk_mesh` dirty region for each changed chunk.

This lets save/load regenerate baseline sunlight and emitter light while preserving compatibility
with existing voxel edit deltas.

## Meshing

Cube and box faces use the maximum of the owning block's light and the exposed neighboring cell's
light. The owning value keeps emissive blocks bright; the neighboring value lights otherwise
opaque terrain surfaces. Light remains a greedy-mesh merge boundary.

## Scheduling contract

`chunk_lighting` dirty regions are the trigger for authoritative relighting. The scheduler layer
incrementally copies immutable snapshots under a cell budget, solves off-thread, rejects stale
output, and applies complete patches on the owner thread. Mesh work remains a separate consumer of
`chunk_mesh` regions.
