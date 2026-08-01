# Voxel meshing experiments

Status: occupancy-assisted source enumeration and measured mesh-capacity tuning are implemented;
face-mask construction and edit-to-visible work remain open.

This note records the reproducible 32-cubed meshing baseline and the first M4 decisions. The harness
measures immutable neighborhood snapshot construction, the complete reference mesher, a fresh
greedy mesh, and the production-style greedy path with retained output buffers. It validates exact
directional surface parity between the reference and greedy outputs and retains checksums, geometry,
allocated capacity, raw samples, and runtime provenance.

## Reproduction and provenance

The retained measurements were produced with:

```text
cmake -S . -B build/voxel-storage-release
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
| Source state | clean tracked tree in every retained run |

Raw artifacts are retained outside Git under `build/voxel-storage-release/benchmarks/` as
`voxel-meshing-01e69e4-run{1,2,3}.json`, `voxel-meshing-f6322d7-run{1,2,3}.json`,
`voxel-meshing-099739e-run{1,2,3}.json`, and `voxel-meshing-350aa2e-run{1,2,3}.json`.

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

The representative 4 ms P95 gate is narrowly missed by sparse caves, and the 10 ms adversarial
gate is missed by the checkerboard. M4 is not complete.

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

## Decision and next experiment

The versioned occupancy mask and surface-bound reservation remain in production. They preserve
reference/greedy surface parity, rich-model/fluid/AO behavior, deterministic output ordering, and
snapshot immutability. Empty rejection is a decisive latency win and the fixed 4 KiB mask is also
available to later collision, lighting, and visibility experiments.

The next meshing experiment is a render-table-revision-coupled full-occluder/greedy-cube mask used
to construct directional face candidates with word operations. It must include snapshot-build cost,
pool any variable scratch storage, retain directional occlusion and AO semantics, and improve total
snapshot-plus-mesh P95. Slab or microbrick rebuilds remain deferred until edit-to-visible traces show
that whole-chunk invalidation, rather than face construction, is the limiting cost.
