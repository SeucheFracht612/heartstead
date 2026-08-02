# Voxel meshing experiments

Status: occupancy-assisted source enumeration, revision-coupled packed face candidates, measured
mesh-capacity tuning, and bounded invalidation-to-resident latency instrumentation are implemented.
The representative, adversarial, and local-edit M4 gates pass on the declared reference CPU.

This note records the reproducible 32-cubed meshing baseline and the first M4 decisions. The harness
measures immutable neighborhood snapshot construction, the complete reference mesher, a fresh
greedy mesh, and the production-style greedy path with retained output buffers. It validates exact
directional surface parity between the reference and greedy outputs and retains checksums, geometry,
allocated capacity, raw samples, and runtime provenance.

## Reproduction and provenance

The retained measurements were produced with:

```text
cmake -S . -B build/voxel-storage-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER=/usr/bin/clang++ \
  -DHEARTSTEAD_ENABLE_VULKAN=OFF
cmake --build build/voxel-storage-release --target heartstead_voxel_meshing_benchmark -j2
build/voxel-storage-release/apps/voxel_meshing_benchmark/heartstead_voxel_meshing_benchmark \
  --output build/voxel-storage-release/benchmarks/voxel-meshing-<commit>-runN.json
```

Each process measures all seven deterministic 32-cubed corpora with three warmups and fifteen
retained repetitions per operation, for 420 raw samples. Values discussed below are the median of
the three process-level medians or the median of their process-level P95 values.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | Clang 18.1.3, Release |
| Original baseline | `01e69e46b86fc08386ec1c5251212b883b86aa59` |
| Occupancy consumer | `f6322d7c5d69f6190357544d47eb73e6a986324a` |
| Dense scan path | `099739ee6a9139aee412f1ef90e19180518c20e9` |
| Surface-bound reserve | `350aa2e0451d3d60c8ec56c6425ed7dffa88f861` |
| Packed meshing masks | `78d246f4b6f5f38a373d7fd70a43cb2b898ad8c0` |
| Isolated-cube fallback | `66271cbd8e82ff85f570bb3be685fb7c903d2c08` |
| Source state | clean tracked tree in every retained run |

Raw artifacts are retained outside Git under `build/voxel-storage-release/benchmarks/` as
`voxel-meshing-01e69e4-run{1,2,3}.json`, `voxel-meshing-f6322d7-run{1,2,3}.json`,
`voxel-meshing-099739e-run{1,2,3}.json`, `voxel-meshing-350aa2e-run{1,2,3}.json`,
`voxel-meshing-78d246f-run{1,2,3}.json`, and
`voxel-meshing-66271cb-run{1,2,3}.json`.

CPU frequency, the desktop workload, power policy, and thermal state were not controlled. Several
short operations and later P95 samples show material run-to-run noise. Decisions therefore use
large effects, absolute time, exact memory, and output parity. A low-single-digit timing movement is
not treated as a result.

## Baseline

The original benchmark established two different reasons to retain greedy meshing. It is slower
than the readable reference implementation on this corpus, but it sharply reduces output geometry
for coherent terrain. Conversely, adversarial checkerboards cannot merge and remain expensive.

| Corpus | Reference median (ms) | Reused greedy median (ms) | Reference faces | Greedy faces |
| --- | ---: | ---: | ---: | ---: |
| Empty | 0.073 | 0.121 | 0 | 0 |
| Uniform solid | 1.074 | 1.315 | 6,144 | 6 |
| Layered terrain | 0.790 | 0.918 | 4,736 | 34 |
| Sparse caves | 3.646 | 4.194 | 25,778 | 25,576 |
| Lit settlement | 1.031 | 1.435 | 8,320 | 8,207 |
| Checkerboard | 9.107 | 12.036 | 98,304 | 98,304 |
| High entropy | 1.272 | 1.681 | 6,144 | 6,144 |

At this baseline, the representative 4 ms P95 gate was narrowly missed by sparse caves and the
10 ms adversarial gate was missed by the checkerboard.

## Revision-coupled occupancy mask

Each production `VoxelChunk` now maintains a fixed 32,768-bit occupancy mask alongside dense cells.
Single-cell edits update one bit; bulk loads and fills rebuild it; light-only changes retag the
unchanged mask. Its revision must exactly equal the content revision. Immutable meshing snapshots
copy the mask, validate its revision, and use set-bit order to enumerate non-air source voxels and
calculate required halo size. Fully occupied chunks retain a dense traversal path. Neighbor cells
are still copied in full because light stored in air cells affects visible face lighting.

The mask costs exactly 4,096 payload bytes per resident chunk and meshing snapshot. The first three
clean occupancy-consumer runs showed:

| Case | Baseline | Occupancy consumer | Change |
| --- | ---: | ---: | ---: |
| Empty reference median | 0.073 ms | 0.0002 ms | -99.7% |
| Empty reused greedy median | 0.121 ms | 0.0003 ms | -99.7% |
| Sparse-cave snapshot median | 0.076 ms | 0.050 ms | -34.9% |
| Sparse-cave snapshot P95 | 0.092 ms | 0.054 ms | -41.2% |
| Sparse-cave reused greedy median | 4.194 ms | 4.092 ms | -2.4% |
| Lit-settlement reused greedy median | 1.435 ms | 1.401 ms | -2.4% |

Full and high-entropy chunks initially regressed by roughly 2–6% because bit enumeration could not
skip cells. The explicit full-mask path restores dense source traversal. Differences at that size
remain close to the uncontrolled benchmark noise floor and do not justify a more complex density
threshold.

Opacity is deliberately not stored in `VoxelChunk`: whether a type or face occludes depends on the
active block render table, not only chunk content. A future opacity/face mask must be an immutable
derived product keyed by both content dependencies and render-table revision.

## Surface-bound mesh reservation

The previous greedy mesher reserved `min(cube_count * 2, 16,384)` quads. A uniform chunk that emits
only six quads consequently retained about 3 MiB of output capacity. The replacement builds a
temporary cube bitset while source cells are already being classified and counts adjacent cube
pairs with row bit operations. `6 * cube_count - 2 * adjacent_pairs` is a conservative upper bound
on emitted unit faces; non-cube and neighbor occlusion can only lower it. The existing 16,384-quad
cap remains for adversarial data.

Allocated output memory changed as follows. These byte counts are deterministic across the retained
runs and include the mesh object and vector capacities.

| Corpus | Original greedy bytes | Surface-bound bytes | Change |
| --- | ---: | ---: | ---: |
| Uniform solid | 3,014,836 | 1,130,676 | -62.5% |
| Layered terrain | 2,622,784 | 759,104 | -71.1% |
| Sparse caves | 6,126,224 | 6,126,224 | unchanged |
| Lit settlement | 2,909,056 | 1,618,816 | -44.4% |
| Checkerboard | 24,437,180 | 24,437,180 | unchanged |
| High entropy | 2,840,368 | 1,201,968 | -57.7% |

Compared with the immediately preceding dense-path build, meshing medians moved by about -0.3% to
+2.7% outside the sub-microsecond empty case. That is below the roadmap's 5% investigation trigger
and buys a large reduction in retained memory. The change is accepted.

## Revision-coupled face candidates

Immutable neighborhood snapshots now derive two render-dependent masks while their cells are
copied: a 32-cubed greedy-cube source mask and a halo-padded full-occluder mask. The product records
the exact center content revision and block-render-table revision; the containing snapshot retains
all center and neighbor dependency revisions. A mismatched table revision is rejected before
meshing. Variable word storage is pooled with the bounded snapshot cell buffers and returned on
success, cancellation, or scheduler-side rejected submission.

The mask/candidate split follows the hidden-face-culling stage in
[Binary Greedy Meshing v2](https://github.com/cgerikj/binary-greedy-meshing), whose padded binary
columns cull many faces per word before merging. Heartstead adopts only that candidate-generation
shape: its material, light, state, directional occlusion, AO, specialized geometry, and conventional
vertex output make the demo's binary-opacity and packed-vertex assumptions inapplicable.

The common full-or-open occlusion path constructs directional candidate rows from the two masks.
Y/Z rows use AND-NOT operations directly, X rows extract aligned padded rows, and set-bit iteration
visits only exposed source faces. Voxel type, state, material, render phase, face light, and AO remain
in the existing face key and merger. AO full-occluder queries now read the padded bitset rather than
repeating block-table lookups. If any captured cell has a nonzero partial directional occlusion mask,
the snapshot records that fact and the mesher uses the previous per-cell directional path. Tests
cover that fallback, cross-chunk culling, cross-chunk AO, stale-table rejection, output parity, and
buffer recycling.

For the normal one-cell halo, the derived payload is 9,016 bytes: 4,096 bytes of greedy-cube bits and
4,920 bytes for 39,304 padded full-occluder bits. Total snapshot payload rises from 477,688 to 486,704
bytes (+1.9%). Empty and specialized-only chunks allocate no mask payload. Voxel-meshing benchmark
schema v2 reports derived-mask payload and retained capacity separately.

The checkerboard exposed a second measured fact: it contains no face-adjacent greedy cubes. In that
case no two same-direction faces can share a tangent edge, so greedy merging is provably unable to
reduce the unit-quad output. The production entry point now selects the reference culled emitter for
that exact condition, unless partial directional occlusion requires the greedy fallback. The culled
emitter accepts and clears the scheduler's recycled `ChunkMesh`, retaining established vertex,
index, section, and rich-instance capacity instead of allocating a fresh adversarial output each
time. Its isolated output is byte-for-byte equal to the direct reference output.

The table below compares the median of three combined process medians and the median of three sums
of process-level P95 values. “Combined” is the conservative sum of independently measured snapshot
rebuild and reused production-mesher statistics.

| Corpus | Previous combined median (ms) | Final combined median (ms) | Previous combined P95 (ms) | Final combined P95 (ms) | P95 change |
| --- | ---: | ---: | ---: | ---: | ---: |
| Uniform solid | 1.467 | 1.018 | 1.634 | 1.107 | -32.3% |
| Layered terrain | 1.006 | 0.780 | 1.100 | 0.862 | -21.6% |
| Sparse caves | 4.178 | 3.322 | 4.278 | 3.524 | -17.6% |
| Lit settlement | 1.430 | 1.270 | 1.530 | 1.332 | -12.9% |
| Checkerboard | 12.322 | 7.685 | 12.762 | 8.549 | -33.0% |
| High entropy | 1.788 | 1.394 | 1.874 | 1.545 | -17.6% |

Across the three final clean runs, sparse-cave combined P95 is 3.485–3.617 ms and checkerboard
combined P95 is 8.291–8.569 ms. Both M4 meshing gates pass. The empty path remains approximately
0.03 ms combined and carries no derived-mask payload.

## Edit-to-visible mesh latency

Every consumed chunk-mesh dirty region now carries a `steady_clock` timestamp. The renderer tracks
that timestamp against the exact requested mesh-stage revision for each affected loaded chunk. This
is intentionally a stage revision rather than only the chunk's content revision: an edit on a chunk
boundary must also time the neighboring remesh even though that neighbor's own voxels did not
change.

The interval completes only after the GPU chunk cache accepts the current upload, the matching
stage ticket is published as resident, and mesh dirty state is cleared. The replacement can then be
selected by the same frame's draw-list construction. This is a CPU-side
invalidation-to-resident/eligible-to-draw measure; it does not claim scan-out or photon latency.

Repeated invalidations for one loaded chunk retain the earliest timestamp and newest required
stage revision. Superseded work therefore cannot prematurely close the interval. Eviction,
distance suppression, reload, or memory-pressure cancellation abandons the pending interval and is
counted separately. Derived lighting/fluid mesh invalidations use the same contract, so results are
best interpreted per benchmark workload rather than as user-input time alone.

Runtime storage is bounded to one pending record per affected chunk and a fixed 256-sample rolling
window. `ChunkRenderStats` and `RendererStats` expose per-frame completion count/maximum, pending
count, latest latency, rolling median/P95/P99/maximum, session maximum, and cumulative completed,
coalesced, and abandoned counts. Tracy plots retain pending count and rolling P95. Renderer
benchmark JSON and CSV schema v4 preserve those counters and distributions in raw frame output and
the summary. Schema v4 resets the observation window after warmup, drains pending intervals without
mixing drain frames into frame-time statistics, retains censored final work, and reports exact
completed-job/built-mesh/published-mesh amplification.

## Decision

The versioned occupancy mask, surface-bound reservation, revision-coupled meshing masks, and
isolated-cube culled fallback remain in production. They preserve reference/greedy directional
surface parity, rich-model/fluid/AO behavior, deterministic per-strategy output, snapshot
immutability, stale rejection, and bounded buffer ownership. Empty rejection is a decisive latency
win and the fixed 4 KiB chunk occupancy mask remains available to later collision, lighting, and
visibility experiments.

The clean three-run `rapid-edits` baseline now passes the 50 ms local-response P95 gate at
19.491–19.854 ms with exactly 1.000 built mesh per publication, zero final pending or abandoned
intervals, and no drain frames; full configuration and raw paths are retained in
[Renderer benchmarks](renderer_benchmarks.md#voxel-rapid-edit-baseline--2026-08-01). This does not
justify slab or microbrick rebuild complexity for normal edits.

The combined sparse-cave and checkerboard P95 gates now pass as well. M4 is complete on the declared
reference CPU. Slab or microbrick rebuilds remain deferred until a future edit trace shows that
whole-chunk invalidation, rather than face construction, is again the limiting cost. Work proceeds
to the bounded asynchronous dynamic-world stages in M5 rather than adding unmeasured meshing
complexity.
