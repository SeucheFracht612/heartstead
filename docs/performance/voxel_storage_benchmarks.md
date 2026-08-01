# Voxel storage experiments

Status: measured experiment; production `VoxelChunk` storage is unchanged.

This note records the first storage decision for milestone M3. The benchmark compares the current
9-byte dense `VoxelCell` array, split structure-of-arrays channels, and an experimental adaptive
section. The adaptive section uses a palette plus arbitrary-width packed indices while that block
channel is smaller than a split-dense `{type, state}` channel. It switches irreversibly to the
split-dense channel when the packed payload would be larger. Light remains uniform or dense and
metadata remains sparse in either mode.

## Reproduction and provenance

The retained measurements were produced with:

```text
cmake -S . -B build/voxel-storage-release
cmake --build build/voxel-storage-release --target heartstead_voxel_benchmark -j2
build/voxel-storage-release/apps/voxel_benchmark/heartstead_voxel_benchmark \
  --output build/voxel-storage-release/benchmarks/voxel-storage-<commit>-runN.json
```

Each process measured all seven deterministic corpora at 16 cubed and 32 cubed, with two warmups,
nine retained repetitions, eight iterations per sample, 256 existing-value edits per iteration,
and 64 novel-value edits. Values below are the median of the three process-level medians. Each raw
file contains 2,772 samples, 308 summaries, 14 memory records, and the exact configuration.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | Clang 18.1.3, Release |
| Palette-only commit | `d601345c90385aaeafd8a346e98bdce691e890cb` |
| Adaptive commit | `69be5dbea2cea4692793e7495f0e2a69fc1b590b` |
| Source state | clean tracked tree in all retained runs |

Raw artifacts are retained outside Git under `build/voxel-storage-release/benchmarks/` as
`voxel-storage-d601345-run{1,2,3}.json` and
`voxel-storage-69be5db-run{1,2,3}.json`.

The machine was otherwise idle, but CPU frequency, power policy, and thermal state were not pinned.
Tiny operations consequently show noise between runs. Decisions use large structural differences,
absolute latency, and memory as well as ratios; a small single-digit percentage is not treated as a
result from this data.

## Result 1: a palette-only live format is rejected

Palette packing saves substantial memory on ordinary sections, but the first implementation used a
linear palette search for edits. A 32-cubed high-entropy section had 28,891 distinct block values.
That produced catastrophic edit behavior even though scans stayed fast.

| 32-cubed high-entropy operation | Dense (ns/item) | Palette-only (ns/item) | Palette/dense |
| --- | ---: | ---: | ---: |
| Type scan | 0.87 | 0.84 | 0.96x |
| Random full-cell read | 3.48 | 12.99 | 3.73x |
| Existing-value edit | 3.31 | 5,775.05 | 1,746x |
| Encode | 0.16 | 41.02 | 254x |
| Decode | 0.16 | 11.32 | 71x |
| Novel-value edit | n/a | 4,689.73 | n/a |

The palette-only format is therefore not a valid universal live representation. A favorable memory
number does not compensate for unbounded palette-dependent edit cost.

## Result 2: adaptive split-dense fallback removes the cliff

The fallback selects split-dense block values for both high-entropy sizes. Existing and novel edit
latency improves by roughly two orders of magnitude while allocated memory also decreases relative
to the oversized packed representation.

| High-entropy case | Palette-only | Adaptive | Change | Dense baseline | Adaptive/dense |
| --- | ---: | ---: | ---: | ---: | ---: |
| 16-cubed existing edit (ns/edit) | 782.07 | 12.81 | -98.4% | 3.76 | 3.41x |
| 16-cubed novel edit (ns/edit) | 721.17 | 12.09 | -98.3% | n/a | n/a |
| 16-cubed allocated bytes | 30,864 | 24,744 | -19.8% | 49,176 | 0.50x |
| 32-cubed existing edit (ns/edit) | 5,775.05 | 20.25 | -99.6% | 3.36 | 6.03x |
| 32-cubed novel edit (ns/edit) | 4,689.73 | 21.56 | -99.5% | n/a | n/a |
| 32-cubed allocated bytes | 241,808 | 180,392 | -25.4% | 393,240 | 0.46x |

The edit result is stable across the three adaptive runs: 32-cubed existing edits ranged from
20.16 to 21.10 ns/edit and novel edits from 21.19 to 21.73 ns/edit.

The fallback is deliberately based on block-channel payload, not corpus identity or a magic palette
count. For a 32-cubed section it switches when palette values plus packed index words exceed the
131,072-byte split-dense block channel. Edits can trigger the same one-way promotion. Tests preserve
a forced-palette policy so every index width from zero through fifteen bits and machine-word boundary
crossing remain covered.

## Memory across the corpus

These are allocated bytes, including container/object overhead but excluding the separately reported
face-mask payload. Only the high-entropy corpus selects dense block values.

| Edge | Corpus | Dense cells | Split channels | Adaptive | Adaptive mode |
| ---: | --- | ---: | ---: | ---: | --- |
| 16 | Empty | 49,176 | 36,968 | 172 | palette |
| 16 | Uniform solid | 49,176 | 36,968 | 172 | palette |
| 16 | Layered terrain | 49,176 | 36,968 | 5,840 | palette |
| 16 | Sparse caves | 49,176 | 36,968 | 7,592 | palette |
| 16 | Lit settlement | 49,176 | 36,968 | 7,608 | palette |
| 16 | Checkerboard | 49,176 | 36,968 | 9,384 | palette |
| 16 | High entropy | 49,176 | 36,968 | 24,744 | split-dense blocks |
| 32 | Empty | 393,240 | 295,016 | 172 | palette |
| 32 | Uniform solid | 393,240 | 295,016 | 172 | palette |
| 32 | Layered terrain | 393,240 | 295,016 | 45,264 | palette |
| 32 | Sparse caves | 393,240 | 295,016 | 57,768 | palette |
| 32 | Lit settlement | 393,240 | 295,016 | 57,832 | palette |
| 32 | Checkerboard | 393,240 | 295,016 | 66,728 | palette |
| 32 | High entropy | 393,240 | 295,016 | 180,392 | split-dense blocks |

Adding the fallback vector costs 24 bytes per experimental object. This explains the 148-to-172-byte
change for uniform sections; it is negligible at section scale but is included rather than hidden.

## Remaining costs and decision

The adaptive candidate is accepted for continued experiments, not for production adoption. It fixes
the measured edit cliff and retains meaningful memory savings, but high-entropy 32-cubed full-cell
reads are still 3.38x dense, edits 6.03x, encode 95.9x, and decode 61.5x. Much of the encode work is
temporary palette discovery before the threshold is known; full-cell reads and decode also traverse
the split light and sparse metadata channels. These costs need representative end-to-end evidence,
not another microbenchmark-only conclusion.

Production storage remains the dense 32-cubed `VoxelCell` array. In particular:

- no save or replication format changed, so compatibility and recovery behavior are untouched;
- 32-cubed chunks remain the default because the 16/32 comparison has not demonstrated an
  end-to-end streaming, meshing, edit-latency, or memory win;
- the next M3 experiment is content-revision-coupled occupancy/opacity masks and mask-assisted face
  rejection;
- a medium-diversity sweep near the adaptive crossover is required before reconsidering live
  storage, because the current named corpora do not exhaust that region;
- save/replication compatibility tests become mandatory before any selected representation can
  replace `VoxelChunk` storage.

This is a reversible optimization decision: retain the adaptive candidate and raw evidence, reject
the naive universal palette, and leave the shipping representation unchanged until the macro corpus
meets the milestone gate.
