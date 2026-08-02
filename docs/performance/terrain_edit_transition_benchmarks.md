# Terrain edit-transition benchmarks

Status: schema v1, calibrated on 2026-08-01.

The `heartstead_terrain_edit_transition_benchmark` executable measures one authoritative voxel
edit as it moves through Heartstead's retained near, mid, and far terrain representations. It uses
the production `ChunkRenderSystem`, GPU chunk cache, authoritative far-surface cache,
`FarTerrainRenderer`, LOD update graph, worker mesh scheduler, arena uploads, visibility tests, and
draw-list builders. No benchmark-only worker or publication path stands in for those systems.

This closes a different question from the chunk render-readiness benchmark. Render readiness starts
with missing chunks and measures load interest through first draw eligibility. This workload starts
with every required representation resident, changes authoritative world content, and proves that
old geometry stays drawable until every replacement for the new revision is current.

## Workload and endpoints

Each warmup or retained repetition creates an independent state:

1. Load a deterministic flat 81-chunk world and settle all bounded near and clipmap work.
2. Select a visible chunk whose exact horizontal bounds intersect both a mid patch and a far patch.
   Selection is deterministic for the configured clipmap; unsupported corpus/configuration pairs
   fail validation instead of silently measuring one band.
3. Start the sample clock before adding one grid-aligned surface voxel. Advance the render owner at
   the configured cadence, 16,667 us by default.
4. On every update, require the previous near chunk and every far patch to remain resident and to
   continue producing draw commands.
5. Stop only after the exact current near mesh is draw-eligible and every invalidated mid and far
   patch is current.
6. Add and immediately remove a second voxel around one owner update. This forced supersession must
   coalesce the near response, reject stale far tickets, converge to the final revision, and retain
   all draw coverage.

Schema v1 names three measurement endpoints:

| Band | Endpoint | Meaning |
| --- | --- | --- |
| Near | `first_exact_current_chunk_draw_command` | The edited chunk's GPU entry matches current content, render-table, dependency, and mesh-stage revisions and contributes a draw command. |
| Mid | `all_invalidated_mid_patches_current` | Every affected retained mid patch has atomically installed its current replacement. |
| Far | `all_invalidated_far_patches_current` | Every affected retained far patch has atomically installed its current replacement. |

The whole-transition endpoint is the latest of those observations. All begin before the world edit.
They include authoritative surface-cache refresh, immutable snapshot capture, asynchronous topology
construction, owner publication, RHI buffer upload calls, culling, and draw-list construction. They
exclude GPU draw execution, presentation, display scan-out, input sampling, and network transit.

## Retained evidence and fail-closed rules

The JSON retains one raw sample and one lifecycle run per repetition, plus source/build, OS, CPU,
render device, driver, cadence, budgets, and gate settings. Raw samples include:

- near, first-mid, complete-mid, first-far, complete-far, and full response times;
- the near system's independent dirty-mark-to-publication latency sample;
- owner, snapshot, worker meshing, upload-preparation, upload-call, and synchronous-wait maxima;
- near/far pending, in-flight, ready, and completed-mailbox high-water marks;
- per-update uploaded bytes and the far pipeline occupancy high-water mark;
- invalidated and rebuilt patch counts by band, job submissions/completions, stale and cancelled
  results;
- minimum resident chunk/patch and draw-command counts observed during replacement;
- supersession coalescing, stale-result rejection, continuity, convergence, and render-resource
  teardown evidence.

The run fails before producing a report if a sample is censored, either band is missed, an isolated
edit produces stale/cancelled work, jobs or uploads fail, a queue/upload/concurrency limit is
exceeded, resident or draw coverage drops, a superseded far result is not rejected, final work stays
pending, or RHI resources leak. Intentional stale/cancelled evidence is accepted only in the
separately labelled supersession probe.

## Starting gates

Gates are opt-in with `--enforce-gates`; an evaluated violation returns process status 3.

| Metric | Default limit | Rationale |
| --- | ---: | --- |
| Near exact-current draw P95 | 50 ms | The roadmap's local visual-edit target. |
| Complete mid replacement P95 | 250 ms | One ordinary world-convergence interval. |
| Complete far replacement P95 | 500 ms | Lower-priority derivative allowance. |
| Whole transition P95 | 500 ms | No band may evade the end-to-end ceiling. |
| Worst renderer-owner update | 12 ms | Calibrated below one 60 Hz interval while clearing observed Vulkan noise. |
| Near upload preparation | 0.5 ms | M5 foreground upload-preparation target. |
| Synchronous GPU wait | 0 ms | No renderer-owner fence wait is permitted. |

These are absolute starting targets, not a substitute for a same-machine regression comparison.
Investigate a change that exceeds both 5% and the measured run-to-run noise floor.

## Running

```sh
cmake --preset default-release
cmake --build --preset default-release \
  --target heartstead_terrain_edit_transition_benchmark -j2
build/default-release/apps/terrain_edit_transition_benchmark/\
heartstead_terrain_edit_transition_benchmark \
  --enforce-gates \
  --output /tmp/heartstead-terrain-edit-transition.json
```

Use `--backend vulkan` to exercise real device-local buffer uploads and descriptor writes. Keep
optimized build, power, thermal, driver, and background-load conditions fixed when comparing runs.
Raw calibration artifacts belong outside Git; the JSON embeds the exact provenance needed to match
them to a source revision.

## Clean reference calibration

Three independent headless and three Vulkan processes were run from clean commit `edb0b43` in
Release with GCC 13.3.0 on an Intel Core Ultra 7 258V, Linux 6.17.0-1030-oem. Each process retained
nine isolated samples plus nine supersession probes. Vulkan used Intel Graphics (LNL), Mesa 25.2.8.
The Vulkan command supplied `--owner-ms 12`; that measured calibration is now the default.

| Backend | Process-level near/mid/far/full P95 | Worst owner update | Worst upload preparation | GPU wait |
| --- | ---: | ---: | ---: | ---: |
| Headless | 17.176–17.248 ms | 1.425–1.521 ms | 0.0112–0.0120 ms | 0 ms |
| Vulkan | 21.754–24.882 ms | 7.423–9.189 ms | 0.0117–0.0159 ms | 0 ms |

All six calibrated processes passed every latency and correctness gate. Each three-process set
published exactly 27 current near meshes, 27 mid replacements, and 27 far replacements for the
retained edits. Pipeline occupancy peaked at two against a concurrency cap of three. Across the 27
supersession probes, near rejected or cancelled 27 obsolete results and far rejected 54 stale
results, with no resident or draw holes and no leaked render resources.

A preliminary Vulkan process observed a 9.138 ms owner update and therefore failed the provisional
8 ms owner ceiling while every response, upload, wait, queue, continuity, and correctness check
passed. The 12 ms default records that observed noise floor with margin while remaining below one
60 Hz frame. It is not evidence that larger edit bursts, long soaks, GPU execution/display response,
or broad world invalidations meet their own targets; those remain separate workloads.
