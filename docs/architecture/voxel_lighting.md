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

The solver consumes a `VoxelLightSnapshot` containing sorted chunk identities, lighting-stage
tickets, content revisions, and copied cells, plus a compact `VoxelLightBlockTable`. The owner
freezes that metadata for every resident chunk before copying the first cell, so a later edit to an
as-yet-uncopied chunk invalidates the entire snapshot instead of producing a mixed-time field. The
worker never reads the live world while propagating.

Sunlight seeding walks each contiguous loaded vertical chunk stack from top to bottom. The upper
boundary of each loaded stack is treated as sky; a missing chunk elsewhere is a closed propagation
boundary, not implicit air. A stable highest-light-first queue performs the six-neighbor flood fill
for sunlight and block light. Stable chunk/local address ordering makes results independent of
chunk insertion order.

The result contains complete, revision-tagged chunk patches and retains every request ticket even
when solving is cancelled. The owner thread validates the complete topology, source/table
revisions, content revisions, and all lighting tickets before applying any patch. It transitions
every exact ticket through `running` and `ready`, then publishes every patch together as
`resident`; unchanged light bytes advance the resident request without advancing the lighting
output revision. A generation or revision mismatch rejects the whole result as stale.

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

The default owner-thread snapshot budget is 49,152 cells per simulation update: one and a half
32-cubed chunks. A controlled 60 Hz sweep selected this point because it reduced calibrated 3x3
relight P95 from 1,267.312 ms at the old 4,096-cell budget to 167.449 ms while avoiding the higher
owner maximum observed at 65,536 cells. The solver owns one background job at a time and coalesces
further edits into a follow-up rebuild. A topology, content, palette, or external-source change
advances the field tickets, makes partial snapshots and completed results stale, and cooperatively
cancels an obsolete solve. Complete field application is timed against a 2 ms budget and records
overruns rather than publishing a partially updated connected light field.

`ChunkLightSystemStats` exposes snapshot backlog, copied cells, queue visits, solve/apply time,
changed chunks/cells, stale work, budget overruns, pending response, completed response count,
coalesced/abandoned invalidations, and a bounded convergence-latency distribution.
`Renderer::set_voxel_lighting_stats` mirrors the current values into `RendererStats`; `dev_game`
does this every frame.

A changed light patch is replicated to connected clients through a newer revision of the affected
chunk's bounded binary snapshot slices. This favors correctness and bounded reuse over a
specialized light-delta codec.

Fire prototypes retain their existing 8-bit field. Legacy fire values in the conventional `0..15`
range are expanded to the full voxel-light range when they become spatial sources. A fire is
spatial when its `fire_id` owns a build piece, entity, or physical resource transform.

## Day/night presentation

`evaluate_day_night` maps the authoritative `WorldTick` and configured ticks-per-day onto a
normalized, wrapping solar orbit. Tick zero is midnight by default, with dawn at one-quarter day,
solar noon at one-half day, and dusk at three-quarters day. Non-24-hour calendars use the same
normalized phase.

The evaluator produces one `RenderEnvironmentData` value containing the sun direction and
intensity, ambient term, horizon/fog color, and fog distances. Dawn and dusk use a smooth
elevation-based twilight band, so the value is continuous at both horizons and at day wrap.
`dev_game` evaluates it from `RuntimeFrameStats::authoritative_world_tick`, never wall-clock time.

The renderer's sky phase owns a fullscreen triangle. Its fragment shader interpolates from the
environment's horizon/fog color to a derived zenith color before terrain is drawn. Sky, terrain,
static instances, debug geometry, and UI retain the existing 128-byte frame push-constant contract;
this stays within Vulkan's portable minimum push-constant size.

The renderer benchmark runs the same `ChunkLightSystem` over every scene, waits for initial
lighting and meshing to settle, then advances one bounded lighting update per simulation frame and
feeds that update's telemetry into `RendererStats`. It does not synchronously drain a whole relight
inside a measured frame.
JSON/CSV output includes solve/apply p50 and p95, maximum backlog and visited cells, changed chunks,
stale results, and apply-budget overruns.

The separate [Voxel response benchmark](../performance/voxel_response_benchmarks.md) measures the
dirty-region timestamp through complete lighting-stage publication at a real 60 Hz owner cadence;
it fails closed when the field becomes idle without a retained convergence sample.
