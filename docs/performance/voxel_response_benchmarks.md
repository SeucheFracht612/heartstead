# Voxel response benchmarks

Status: isolated collision-publication and whole-field relight-convergence gates pass on the
declared reference CPU with both the deterministic headless physics backend and Jolt. This is not
an edit-to-display, burst-edit, streaming, replication, mesh, or GPU-upload closure.

The `heartstead_voxel_response_benchmark` executable drives the production `ChunkDatabase`,
`DirtyRegionTracker`, `ChunkCollisionSystem`, `ChunkLightSystem`, collision cooker, light solver,
and selected physics-body backend. It retains every response sample and the source/build/machine
provenance needed to distinguish worker completion from exact owner-thread publication.

## Timing contract

The benchmark builds a square 3x3 corpus of 32-cubed chunks. Every chunk has one opaque ground
layer and air above it. After the initial collision and light fields settle, paired operations add
and remove one interior solid voxel at y=1. Each operation is state-changing and the next operation
is not issued until both systems are fully idle and every collision and lighting stage is current.

The edit marks collision and lighting dirty regions using `steady_clock`. That shared invalidation
timestamp starts both response clocks:

- collision response ends only when the exact chunk identity and current collision-stage request
  publish into the selected physics world; unchanged geometry still has to publish the request;
- relight convergence ends only after a complete topology-consistent light field has solved,
  every current lighting ticket has applied, and the whole field is resident;
- the first collision/light owner update occurs immediately after the edit, matching the runtime
  update order, and later updates follow a fixed 16,667 us cadence (approximately 60 Hz);
- owner-update time covers both production systems and dirty-region handoff. Worker solve time,
  cooking time, apply time, copied cells, changed cells, and stale work remain separately
  attributable.

Two warmup edits are discarded. Both fixed-capacity runtime latency windows are then reset before
the nine retained edits; report percentiles use linear interpolation over those raw values. This is
deliberately a sustainable isolated-response workload. It does not model an open arrival stream or
claim to measure queueing under a burst. The streaming benchmark uses a fixed target population for
that different coordinated-omission concern; see
[Chunk streaming benchmarks](chunk_streaming_benchmarks.md).

## Fail-closed invariants and gates

The report is invalid if any configured edit is absent, non-finite, duplicated, or censored. The
run also fails on timeout, system error, coalesced or abandoned measured invalidation, more than one
completion per edit, pending work, a non-current final stage, or a response without exactly one
latency sample. Those correctness conditions apply even without `--enforce-gates`.

The default performance gates are:

| Metric | Default limit |
| --- | ---: |
| Exact collision-stage publication P95 | 100 ms |
| Complete resident-field relight convergence P95 | 250 ms |

`--enforce-gates` evaluates both limits. A performance violation returns exit code 3 after writing
the complete JSON report. `--physics-backend headless|jolt` selects the body-publication backend;
the report records that choice. A requested unavailable backend fails during configuration instead
of silently falling back.

The default collision scheduler uses one worker, two concurrent jobs, two submissions and two
applies per owner update, and a 2 ms apply-loop budget. Lighting uses one worker, one complete field
at a time, a 49,152-cell snapshot budget per owner update, and a 2 ms complete-field apply budget.
The apply budget records overruns; the field is never partially published merely to meet it.

## Reproduction and provenance

The retained calibration was produced with:

```text
cmake --preset default-release
cmake --build --preset default-release --target heartstead_voxel_response_benchmark -j2
build/default-release/apps/voxel_response_benchmark/heartstead_voxel_response_benchmark \
  --physics-backend BACKEND \
  --enforce-gates \
  --output build/default-release/benchmarks/heartstead-voxel-response-4632686-BACKEND-runN.json
```

`BACKEND` was `headless` and `jolt`; `N` was 1, 2, and 3 in separate sequential processes. Every
process used two warmups, nine retained edits, the default 3x3 corpus, a 60 Hz owner cadence, and the
49,152-cell snapshot budget.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `463268698550c32622a21d6aa2296f2aad8f9dd7` |
| Source state | clean tracked tree in every retained run |

Raw reports remain outside Git under `build/default-release/benchmarks/` as
`heartstead-voxel-response-4632686-{headless,jolt}-run{1,2,3}.json`.

## 2026-08-01 calibration

| Backend and metric | Run 1 P95 | Run 2 P95 | Run 3 P95 | Median process P95 | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Headless collision publication | 16.855 ms | 16.780 ms | 16.776 ms | 16.780 ms | 100 ms |
| Headless relight convergence | 167.719 ms | 167.514 ms | 167.314 ms | 167.514 ms | 250 ms |
| Jolt collision publication | 16.871 ms | 16.815 ms | 16.919 ms | 16.871 ms | 100 ms |
| Jolt relight convergence | 167.498 ms | 167.565 ms | 167.442 ms | 167.498 ms | 250 ms |

Across the six processes and 54 retained edits, every edit produced exactly one collision and one
relight sample. There were zero stale results, stale snapshots, failed results, coalesced measured
invalidations, abandoned invalidations, final pending responses, and relight apply-budget overruns.
The mean response used 10.89 to 11 owner updates per edit.

The worst headless owner update was 4.643 ms; the worst Jolt update was 4.526 ms. Across both
backends, collision cooking stayed below 0.136 ms, collision apply below 0.139 ms, light solve below
76.651 ms, and complete-field light apply below 0.957 ms. CPU frequency, desktop workload, power
policy, and thermal state were not controlled, so these values are a local absolute-gate
calibration rather than a portable hardware guarantee.

## Snapshot-budget tuning

A clean headless Release sweep on revision `0e111bd94dbf4d47e8b7744d53f7029da491e52d`
held the corpus, edit sequence, solver, 60 Hz cadence, warmups, and repetitions constant while
changing only the lighting snapshot copy budget:

| Cells/update | Relight P95 | Mean owner updates/edit | Worst owner update | Result |
| ---: | ---: | ---: | ---: | --- |
| 4,096 | 1,267.312 ms | 77.00 | 2.645 ms | Fails 250 ms gate |
| 32,768 | 217.615 ms | 14.00 | 3.836 ms | Passes, limited tail margin |
| 49,152 | 167.449 ms | 10.89 | 4.212 ms | Selected default |
| 65,536 | 150.792 ms | 10.00 | 5.050 ms | One frame faster, higher owner cost |

The old 4,096-cell default made a 294,912-cell field spend 72 copy updates before its worker solve
could begin. The 49,152-cell setting copies one and a half chunks per update, removes 66 copy
updates from this corpus, and retains roughly 82 ms of relight-gate headroom on the calibration
machine. Raising the budget to 65,536 saved only one additional 60 Hz update while increasing the
observed owner maximum, so it was rejected as a worse latency/cost tradeoff. Sweep reports remain
outside Git beside the retained calibration.

## Boundary and remaining M5 work

This benchmark proves isolated dirty-region-to-publication response for a resident 3x3 field. The
Jolt run includes terrain-body acceptance and replacement, but it does not move a character or
measure contact response. The light gate rebuilds all resident chunks and therefore measures the
current complete-field algorithm only at this corpus size; larger residency and edit-burst scaling
remain separate workloads.

The benchmark stops before replication transport, client residency, remeshing, upload,
draw eligibility, display scan-out, and newly requested chunk loading. The separate
[chunk render-readiness benchmark](chunk_render_readiness_benchmarks.md) now closes generated
required-chunk loading through exact current draw-command construction, including meshing and RHI
upload. The separate [chunk streaming benchmark](chunk_streaming_benchmarks.md) now gates warm and
Linux cache-drop-advice reads through the production indexed save reader. Save-under-streaming,
large snapshot capture, coordinated background persistence checkpoint,
guaranteed-cold/multi-filesystem behavior, general generated-world loader adoption, and actual GPU
execution/presentation/display timing remain M5 work in the
[Voxel optimization roadmap](voxel_optimization_roadmap.md). Durable per-chunk append and full
checkpoint scale are covered separately by the
[chunk delta journal benchmark](chunk_delta_journal_benchmarks.md).
