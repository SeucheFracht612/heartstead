# Voxel optimization roadmap

Status: maintained implementation plan derived from the optimization research brief and an audit
of the current engine on 2026-08-01.

This roadmap turns the research brief into ordered, testable changes for Heartstead. It is not a
promise to implement every technique named by the report. Each optimization must improve a measured
shipping workload, preserve correctness, and retain a simpler fallback where hardware or content
requires one.

## Operating rules

1. Instrument before optimizing and retain raw samples, provenance, and traces.
2. Keep queues, memory, publication work, uploads, simulation, and network traffic bounded.
3. Give chunk content and every derived product an independently testable version.
4. Reject stale work at publication; use cancellation checkpoints to avoid completing obsolete
   expensive stages.
5. Optimize editable near terrain for locality. Treat far terrain as a derived representation.
6. Compare macrobenchmark P95/P99 as well as microbenchmark throughput and memory.
7. Adopt advanced GPU or sparse structures only when a captured bottleneck justifies their cost.

## Audited baseline

| Research capability | Current Heartstead state | Planned action |
| --- | --- | --- |
| Permanent hierarchical profiling | Partial: retained CPU/GPU timers, counters, raw benchmark frames, and opt-in Tracy zones now cover major runtime, renderer, chunk, worker, lighting, collision, and streaming paths. | Extend zones and attribution as later stages are changed; add allocation ownership and queue-age plots. |
| Deterministic macrobenchmarks | Strong renderer catalog with representative and adversarial voxel, edit, streaming, lighting, fluid, particle, material, and environment scenes. The live renderer-proof test covers rapid interest teleports, cancellation, convergence, and zero-reservation teardown. Separate open-loop chunk, isolated voxel-response, and end-to-end render-readiness benchmarks retain request-to-resident, collision-publication, whole-field relight, upload, and exact draw-command percentiles. | Add server/client, save, cold-start, burst-edit, physical-disk, actual GPU execution/presentation, and long-soak workloads. |
| Reproducible provenance and gates | Renderer schema v4, chunk-streaming schema v2, voxel-response schema v1, and chunk-render-readiness schema v1 record source/build/CPU/device/run metadata, warmups, repetitions, raw samples, workload configuration, and fail-closed lifecycle invariants. Optional gates cover frame distributions, uploads, available GPU, rapid-edit mesh response, generated/saved resident publication, exact collision publication, full-field relight convergence, required-chunk draw eligibility, synchronous GPU waits, mesh amplification, and owner publication time. | Add relative-regression checks and the remaining physical-I/O, display, persistence-scale, and multiplayer gates. |
| Bounded jobs and cancellation | Generic and typed schedulers now bound pending/result work, expose backpressure and queue-age telemetry, age priorities, and support reasoned queued/cooperative cancellation. | Attribute per-type saturation in higher-level pipeline counters and tune limits from traces. |
| Versioned chunk pipeline | An owner-thread ledger now separates content, light, mesh, collision, persistence, and replication request/output revisions and states. Save/replication, mesh/GPU, collision/physics, and whole-field lighting publication are ticket-validated across edit and reload races. | Calibrate stale-work amplification and latency under representative edit/streaming traces. |
| Compact voxel sections | Chunks remain fixed 32³ with contiguous dense `VoxelCell` production storage. Reproducible 16/32 experiments now cover dense, split, palette-packed, uniform-light, sparse-metadata, and adaptive split-dense fallback candidates. | Retain dense production storage while mask/macro work proceeds; add a medium-diversity crossover sweep before any storage selection. |
| Occupancy and opacity masks | A fixed 4 KiB occupancy mask follows the exact chunk content revision. Meshing snapshots also carry pooled greedy-cube and halo-padded full-occluder masks keyed by content dependencies and render-table revision. | Reuse the resident occupancy mask for later measured consumers; keep render-dependent masks derived and consumer-specific. |
| Face culling and greedy meshing | Implemented with immutable neighborhood snapshots, material/render phases, bounded scheduling, stale rejection, pooled buffers, reproducible isolated benchmarks, occupancy-assisted rejection, word-level face candidates/AO queries, surface-bound reservation, an isolated-cube culled fallback, and bounded invalidation-to-resident traces. | Keep slab or microbrick rebuilds deferred unless a future measured edit P95 again exceeds target. |
| Dynamic edit propagation | Dirty regions, neighbor dependencies, asynchronous mesh/light/collision work, upload quotas, exact mesh/collision/relight lifecycle tracking, edit coalescing/abandonment telemetry, and calibrated visual, collision, relight, required-chunk upload-preparation, and draw-eligibility P95 gates exist. | Add burst-edit collision/relight amplification and actual GPU execution/presentation/display response workloads. |
| Streaming and persistence | Interest hysteresis, dirty pinning, deterministic generation, per-chunk indexed delta save/replication, residency budgets, and far clipmaps exist. Durable snapshot acceptance/compaction and application saves run through a bounded save worker. A bounded chunk loader moves disk/decode/generation/private edit application off-thread and is active in the live renderer-proof stream. Saved-delta publication and narrow flushes no longer scan or copy global edit history, and physical delta sources parse the selected generation index once rather than once per requested chunk. | Add physical cache-state/read-scale gates, replace full-table per-chunk writes, adopt async loading in the general generated-world controller, bound eviction waves, and add scale-calibrated save-capture gates. |
| Visibility, LOD, and GPU scaling | Frustum/distance/hierarchical visibility, HZB support, far clipmaps, indirect rendering, GPU arenas, upload staging, and pass timestamps already exist. | Tune only from captures; validate total culling benefit and retain broad fallback paths. |
| Simulation and multiplayer scale | Simulation LOD, server authority, interest management, replication deltas, and fixed-step runtime exist. | Add multi-client spread/convergence benchmarks, byte/time quotas, backlog recovery gates, and soak coverage. |

## Ordered milestones

### M0 — measurement foundation

Deliverables:

- opt-in optimized Tracy build with permanent no-op call sites in ordinary builds;
- hierarchical zones and queue plots at the main engine boundaries;
- benchmark schema v4 with commit, dirty state, build, compiler, OS, CPU, GPU, driver, run
  configuration;
- tier profiles and non-zero process exit on an evaluated budget failure;
- unit and smoke coverage for serialization, provenance, and gate evaluation.

Exit gate: the same run can be attributed to source and machine state, raw frames are retained, and
an absolute budget can fail automation. This milestone is implemented; hardware baselines still
need calibration.

### M1 — bounded work and cooperative cancellation

Deliverables:

- pending and completed limits in the generic job system;
- an explicit submission outcome for backpressure instead of unbounded allocation;
- queued-job cancellation plus coarse polling for running work;
- job timestamps, type, priority, estimated cost, cancellation reason, and queue-age counters;
- starvation and shutdown tests, including a full result mailbox.

Exit gate: all job memory is bounded, urgent work can displace or cancel obsolete speculative work,
and stress tests return every queue to zero. Cancellation remains cooperative because worker tasks
are not safely preemptible. This milestone is implemented; queue limits remain subject to workload
calibration as later pipeline stages are added.

### M2 — explicit chunk-stage versions and publication

Deliverables:

- an owner-thread chunk stage ledger for content, lighting, mesh, collision, persistence, and
  replication revisions;
- immutable job inputs and narrow result publication commands;
- current-version validation before expensive sub-stages and publication;
- per-stage requested/running/ready/resident/stale/cancelled counters;
- deterministic unload/reload/edit race tests.

Exit gate: an obsolete derived result cannot publish, duplicate completed mesh work stays below 1.1
jobs per published mesh during normal edits, and no worker accesses live mutable world state. This
milestone is implemented; stale-work amplification and latency remain workload-calibration tasks
for the later meshing and dynamic-world milestones.

### M3 — voxel storage experiments

Deliverables:

- microbenchmarks for cell scan, random edit, palette lookup, serialization, face-mask construction,
  and representative/adversarial chunk corpora;
- measured dense, palette-packed, and optional uniform/sparse section candidates;
- occupancy/opacity masks coupled atomically to the content revision;
- exact CPU bytes per resident section, non-air voxel, and visible face;
- save and replication compatibility tests for any selected representation.

Exit gate: a selected format beats dense storage on the macro corpus without unacceptable edit or
decode P95. Chunk size remains 32 cubed unless the 16/32 experiment demonstrates an end-to-end win.

Progress: the deterministic corpus, codecs, raw-sample benchmark, exact memory accounting,
adaptive fallback, and production revision-coupled occupancy mask are implemented and documented
in [Voxel storage experiments](voxel_storage_benchmarks.md) and
[Voxel meshing experiments](voxel_meshing_benchmarks.md). The data rejects a universal palette-only
live format and does not yet justify replacing production storage. Consumer-specific,
render-table-dependent meshing masks and macro validation are now implemented; compatibility tests
for any eventually selected replacement live format remain open.

### M4 — meshing and edit latency

Deliverables:

- mask-assisted empty and face rejection where M3 measurements justify it;
- isolated reference/greedy/run construction benchmarks with reusable scratch storage;
- edit burst coalescing and explicit edit-to-visible instrumentation;
- section, slab, or microbrick rebuild experiment only if whole-section P95 exceeds target;
- mesh, upload, and stale-work amplification metrics.

Exit gate: representative section mesh P95 is at most 4 ms and adversarial P95 at most 10 ms on the
declared mainstream reference CPU; local edit visual response P95 is at most 50 ms.

Progress: the isolated reference/fresh/reused benchmark, exact output-memory accounting,
occupancy-assisted empty/source rejection, and surface-bound output reservation are implemented and
documented in [Voxel meshing experiments](voxel_meshing_benchmarks.md). Exact mesh-stage
invalidation-to-resident tracking, a fixed rolling percentile window, warmup reset, bounded tail
drain, mesh-work amplification, and benchmark schema v4 are also implemented. Three clean
`rapid-edits` runs pass at 19.491–19.854 ms P95 with 1.000 build amplification and no censored work;
see [Renderer benchmarks](renderer_benchmarks.md#voxel-rapid-edit-baseline--2026-08-01).
Revision-coupled greedy-cube/full-occluder masks, word-level directional candidates and AO queries,
pooled mask storage, partial-direction fallback, and a pooled culled emitter for provably
unmergeable isolated cubes are implemented. Three clean final runs put combined snapshot-plus-mesh
P95 at 3.485–3.617 ms for sparse caves and 8.291–8.569 ms for checkerboard. All M4 exit gates pass;
slab and microbrick rebuilds remain deferred. Full measurements are in
[Voxel meshing experiments](voxel_meshing_benchmarks.md#revision-coupled-face-candidates).

### M5 — asynchronous dynamic-world pipeline

Deliverables:

- bounded disk read, decode, generation, save serialization/compression, and publication stages;
- save journal with durable acceptance, recovery, and background compaction;
- per-frame item/time limits for light, collision, mesh, upload, and owner-thread publication;
- memory reservations before large decode/generation jobs;
- teleport, save-under-load, and crash-recovery scenarios.

Exit gate: gameplay-thread save work stays below 0.25 ms, upload preparation below 0.5 ms/frame,
player-adjacent collision response below 100 ms, ordinary lighting convergence P95 below 250 ms,
and required resident/predicted chunks reach visibility P95 within 250 ms.

Progress: durable file replacement, checksummed snapshot journal acceptance, crash recovery,
checkpoint compaction, bounded save scheduling, worker-side slot metadata, and application-level
autosave/final-save adoption are implemented. The live chunk-load pipeline now bounds worker,
mailbox, memory-reservation, item-publication, and publication-time work; separates disk, decode,
generation, preparation, and owner publication timings; accepts immutable custom generators; and
rejects cancelled/stale work before publication. The renderer-proof runtime uses this path and its
rapid-teleport test proves cancelled requests drain, off-interest chunks do not publish, server and
client converge on all 441 desired chunks, and reservations return to zero. The focused save/load
and response paths pass warning-as-error builds plus ASan/UBSan and TSAN; the complete suite is
green.

The file-backed saved-delta source now opens and validates one generation-scoped sorted index before
workers use it. Concurrent reads perform `O(log n)` lookup and one payload-file read; they no longer
parse and sort `index.txt` for every requested coordinate. Existing readers remain pinned across a
new generation activation, legacy inline snapshots remain supported, and corrupt indexes fail at
open. Physical cache-state calibration and the full-table streamed-write replacement remain M5
work.

The open-loop chunk benchmark declares every target coordinate at one instant so bounded admission
delay remains in each raw sample. Its saved-delta workload retains 16,384 unrelated edits plus one
obsolete history per target and requires indexed replacement without rebuilding the flat export
view. On the declared reference CPU, three clean Release processes measured median process-level
near-ring P95 at 35.398 ms, teleport target-ring P95 at 37.645 ms, and saved-delta P95 at 37.694 ms,
with a worst owner publication update of 55 us. Correctness gates require complete convergence,
exact obsolete cancellation, no off-interest/stale/failed publication, unchanged saved-edit
cardinality, zero global-view rebuilds, and zero final reservation. See
[Chunk streaming benchmarks](chunk_streaming_benchmarks.md).

The isolated voxel-response benchmark issues paired solid add/remove edits only after collision and
lighting settle, advances the production systems at a 60 Hz owner cadence, and fails closed on
missing/censored samples, coalescing, abandonment, pending work, failures, or non-current final
stages. Three clean headless and three clean Jolt Release processes on the reference CPU measured
collision-publication P95 at 16.776–16.919 ms and complete 3x3-field relight P95 at
167.314–167.719 ms. All 54 retained edits passed the 100/250 ms gates with zero stale work, failed
work, coalescing, abandonment, pending responses, or light-apply budget overruns. A controlled
snapshot-budget sweep replaced the 4,096-cell default, whose relight P95 was 1,267.312 ms, with a
49,152-cell default selected at 167.449 ms P95 and a 4.212 ms worst combined owner update. See
[Voxel response benchmarks](voxel_response_benchmarks.md).

The render-readiness benchmark declares a fixed 13-chunk required zone at one instant and carries
all 117 retained samples per process through the production loader, database, asynchronous mesher,
GPU cache, RHI upload, visibility hierarchy, and draw-list builder. It requires exact current
content/render/dependency revisions and a matching non-empty draw command, while retaining upload
preparation, upload-call, synchronous-fence-wait, owner-time, memory, queue, and stale-work evidence.
Three clean headless processes measured draw-eligibility P95 at 166.856–166.973 ms; three clean
Vulkan processes on Intel Graphics (LNL) measured 166.942–166.956 ms. Upload preparation remained
below 0.045 ms, synchronous GPU wait was zero, and mesh-build amplification was 2.286 against gates
of 250 ms, 0.5 ms, 0 ms, and 2.5 respectively. The endpoint deliberately precedes GPU draw
execution, presentation, and scan-out. See
[Chunk render-readiness benchmarks](chunk_render_readiness_benchmarks.md).

A small release lifecycle sample measured save owner handoff at 0.046 ms, below the 0.25 ms target,
but this is not a scale-qualified closure: snapshot capture still scales with owned world state.
General-world runtime adoption, physical-disk/cache coverage, burst-edit/large-residency response,
load-under-save/large-snapshot-capture benchmarks, and actual GPU execution/presentation/display
timing remain before M5 can be marked complete.

### M6 — world and multiplayer scale

Deliverables:

- measured predictive streaming with hit/waste/cancellation statistics;
- retention hysteresis driven by memory pressure and explicit eviction value;
- near/mid/far representation transition tests after edits;
- simulation temporal-LOD and catch-up budgets;
- per-client/global replication byte and serialization-time quotas;
- multiplayer spread, convergence, rapid traversal, and long-soak benchmarks.

Exit gate: server P99 meets its tick interval, bounded stress backlog clears within two ticks,
streaming queues respect visible-hole deadlines, and memory has no upward slope after caches reach
their caps.

### M7 — trace-gated GPU work

Candidates include further GPU-driven visibility, meshlets, descriptor indexing, compute meshing,
virtualized resources, and sparse far-field structures. Each candidate requires a captured CPU,
GPU, bandwidth, or residency bottleneck; shipping-hardware measurements; correctness and fallback
parity; and a net macrobenchmark improvement. A candidate without that evidence is rejected or
deferred.

## Initial acceptance matrix

| Measure | Starting gate |
| --- | --- |
| Client frame pacing | Tier median; P95 at most 1.25 times interval; P99 at most 1.5 times; no unexplained frame above 3 times interval. |
| Minimum/mainstream/high-end GPU | Mean at most 13.5/9.0/6.7 ms when timestamps are available. |
| Upload burst | At most 2 MiB/frame for minimum and compatibility; 4 MiB/frame for mainstream and high-end. |
| Server tick | P99 at most the tick interval; no sustained backlog. |
| Mesh latency | Representative P95 at most 4 ms; adversarial P95 at most 10 ms. |
| Local edit | Visual P95 at most 50 ms; adjacent collision at most 100 ms. |
| Lighting | Begins within one frame; ordinary convergence P95 at most 250 ms. |
| Near draw eligibility | P95 at most 250 ms for resident or predicted inputs; GPU execution and display require a separate endpoint. |
| Allocation | Zero general-heap allocations in established mesh, visibility, and fixed-tick inner loops. |
| Regression | Investigate a change exceeding both 5% relative and the metric's measured noise floor. |

Tier values are starting engineering targets, not universal hardware guarantees. Gates run in
optimized builds. Build, profiler, validation, and driver state are recorded; operators must also
hold power and thermal conditions constant across comparisons.

## Change and commit policy

Each milestone is split into reviewable commits: contract/tests first when practical, then the
implementation, then measured tuning and documentation. A performance commit records the exact
benchmark commands and retains before/after raw artifacts outside Git when they are large. A local
throughput gain is not accepted if representative macrobenchmark P99, memory, correctness, or
maintainability regresses without an explicit tradeoff decision.
