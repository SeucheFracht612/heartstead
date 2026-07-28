# Voxel Fluids

Voxel fluids are authoritative chunk cells, not renderer effects. A fluid prototype uses
`BlockLogicalOccupancy::fluid`; its compact simulation state is stored in
`VoxelCell::state_bits`, so existing chunk snapshots, replication, edit deltas, and save/reload
retain an in-progress flow without a second persistence format.

## Cell format

The first fluid format reserves bits `0..8` and rejects all other bits:

| Bits | Meaning |
| --- | --- |
| `0..3` | integer amount `1..8` (`8` is a full cell) |
| `4` | falling column |
| `5` | renewable source |
| `6..8` | flow hint: none, -X, +X, -Z, +Z |

Air always remains type `0` with zero state. A source must be full. Fluid cells cannot carry a
rich-block metadata handle. These invariants deliberately make malformed or version-mismatched
saved cells fail closed instead of silently becoming a different fluid.

`VoxelPalette::cell_for` creates a full finite cell for a fluid prototype. Only generation,
validated gameplay, or migration code may add the source bit.

## Ocean baseline

`TerrainGenerationConfig` optionally names a sea level and an ocean fluid prototype. Air above
terrain and at or below that level becomes a full source cell. This makes the reproducible
world-generation baseline an infinite ocean reservoir while player-placed water remains finite.
It does not create water above sea level.

## Simulation model

The runtime model follows the active-queue approach used by
[Luanti's `ServerMap::transformLiquids`](https://github.com/luanti-org/luanti/blob/master/src/servermap.cpp):
edits enqueue local work, a fixed budget advances only active cells, and overflow remains visible
backlog. Heartbound additionally uses immutable tick input and a sorted two-phase proposal/apply
boundary. This mirrors deterministic sort-key playback in
[Unity's entity command buffers](https://github.com/Unity-Technologies/EntityComponentSystemSamples/blob/master/EntitiesSamples/Docs/entity-command-buffers.md):
container or worker iteration order cannot decide a tie.

For each 20 Hz fluid tick, gravity is resolved before lateral equalization. Source cells refill,
finite cells conserve integer volume, and only resident cross-chunk neighbors participate.
Unloaded space is a closed boundary. Changed cells reactivate themselves and their six neighbors;
a quiet frontier is settled and consumes no work.

The frontier is reconstructible rather than saved: loading a chunk containing fluid, or loading a
neighbor beside fluid, activates its fluid cells and six-neighbor halo. The authoritative cells
are sufficient to resume exactly.

## Presentation and interaction

Fluid cells bypass the cube greedy path. The fluid extractor emits:

- a top surface whose four heights sample adjacent levels;
- side faces down to adjacent fluid height;
- no interior faces between equal/full cells;
- state flow direction in vertex UV/state data for material animation.

Character submersion samples the same decoded amount-derived surface height. Swimming uses
submerged fraction for buoyancy and drag rather than treating any contact with the cell as fully
submerged. Dynamic bodies opt in separately and apply buoyancy through the physics boundary.

## Budgets and observability

The default target is 32,768 active cells at 20 Hz with simulation p95 below 2 ms on the published
reference machine. Each call has a hard cell budget. Statistics expose active/backlog cells,
visited cells, changed cells/chunks, settled ticks, and budget exhaustion; work is deferred rather
than extending the authoritative tick.
