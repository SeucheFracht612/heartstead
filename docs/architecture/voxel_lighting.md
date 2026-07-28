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
- active fire records seed the same block-light field at their owning build/entity/resource block
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

The default owner-thread snapshot budget is 4,096 cells per simulation update. The solver owns one
background job at a time and coalesces further edits into a follow-up rebuild. A topology, content,
palette, or external-source change makes partial snapshots and completed results stale. Complete
field application is timed against a 2 ms budget and records overruns rather than publishing a
partially updated connected light field.

`ChunkLightSystemStats` exposes snapshot backlog, copied cells, queue visits, solve/apply time,
changed chunks/cells, stale work, and budget overruns. `Renderer::set_voxel_lighting_stats` mirrors
the current values into `RendererStats`; `dev_game` does this every frame.

Until the M7 binary replication pass, a changed light patch is replicated to connected clients as
a newer revision of the affected chunk's existing bounded snapshot slices. This is intentionally
correct before it is bandwidth-optimal.

Fire prototypes retain their existing 8-bit field. Legacy fire values in the conventional `0..15`
range are expanded to the full voxel-light range when they become spatial sources. A fire is
spatial when its `fire_id` owns a build piece, entity, or physical resource transform.
