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

For each 20 Hz fluid tick, vertical inflow is resolved before lateral spread. A vertical column
uses a full falling level; supported lateral flow decays by one level per cell, giving a source a
seven-cell reach on flat terrain. Source cells refill, finite flowing levels retract when their
upstream support disappears, and only resident cross-chunk neighbors participate. This is a
gameplay level model, not volume-conserving CFD. Unloaded space is a closed boundary. Changed
cells reactivate themselves and their six neighbors; a quiet frontier is settled and consumes no
work.

The frontier is reconstructible rather than saved: loading a chunk containing fluid, or loading a
neighbor beside fluid, activates its fluid cells and six-neighbor halo. The authoritative cells
are sufficient to resume exactly.

## Presentation and interaction

Fluid cells bypass the cube greedy path. The fluid extractor emits:

- a top surface whose four heights sample adjacent levels;
- exposed side skirts down to the cell base;
- no interior faces between equal/full cells;
- state flow direction in vertex UV/state data for material animation.

Character submersion samples the same decoded amount-derived surface height. Swimming uses
the intersected fluid volume, normalized by the controller volume, for buoyancy and vertical drag
rather than treating any contact with the cell as fully submerged. Separate enter/exit thresholds
prevent mode chatter at a shallow surface, and transitions emit gameplay events. Jump ascends,
crouch dives, and the encumbrance movement multiplier remains authoritative. The Jolt controller
continues to resolve terrain and build-piece contact; only the fluid-volume query remains voxel
based. Dynamic bodies opt in separately and apply buoyancy through the physics boundary.

## Budgets and observability

The default target is 32,768 active cells at 20 Hz with simulation p95 below 2 ms on the published
reference machine. Each call has a hard cell budget. Statistics expose active/backlog cells,
visited cells, changed cells/chunks, settled ticks, and budget exhaustion; work is deferred rather
than extending the authoritative tick.

The `active-water` renderer benchmark keeps exactly 32,768 source cells active throughout warm-up
and measurement, exercises fluid surface rendering, and publishes fluid snapshot/simulation/apply
percentiles in both JSON and CSV:

```sh
cmake --build build/default-release --target heartstead_render_benchmark
build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene active-water --radius 0 --warmup 6 --frames 90 \
  --output build/default-release/active-water.json
```

Reference result on 2026-07-28: Intel Core Ultra 7 258V (8 cores), GCC 13.3.0, optimized
`default-release`, headless renderer: 32,768 processed cells, 1.491 ms simulation p95, 0.036 ms
snapshot p95, and 0.001 ms apply p95. Stable source evaluations do not allocate/sort no-op
proposals; this preserves identical state while keeping quiet or unchanged active work bounded.
