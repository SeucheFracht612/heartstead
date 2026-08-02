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
| Deterministic macrobenchmarks | Strong renderer catalog with representative and adversarial voxel, edit, streaming, lighting, fluid, particle, material, and environment scenes. | Add teleport, rapid-traversal time-to-visible, server/client, save, cold-start, and long-soak workloads. |
| Reproducible provenance and gates | Benchmark schema v4 records source/build/CPU/GPU/driver/run metadata, warmup-isolated edit latency, a bounded tail drain, and mesh-work amplification. Optional tier gates enforce median, P95, P99, maximum frame, upload, available GPU, and sustainable rapid-edit limits. | Add calibrated reference-machine baselines, repetitions, relative-regression checks, and non-renderer gates. |
| Bounded jobs and cancellation | Generic and typed schedulers now bound pending/result work, expose backpressure and queue-age telemetry, age priorities, and support reasoned queued/cooperative cancellation. | Attribute per-type saturation in higher-level pipeline counters and tune limits from traces. |
| Versioned chunk pipeline | An owner-thread ledger now separates content, light, mesh, collision, persistence, and replication request/output revisions and states. Save/replication, mesh/GPU, collision/physics, and whole-field lighting publication are ticket-validated across edit and reload races. | Calibrate stale-work amplification and latency under representative edit/streaming traces. |
| Compact voxel sections | Chunks remain fixed 32³ with contiguous dense `VoxelCell` production storage. Reproducible 16/32 experiments now cover dense, split, palette-packed, uniform-light, sparse-metadata, and adaptive split-dense fallback candidates. | Retain dense production storage while mask/macro work proceeds; add a medium-diversity crossover sweep before any storage selection. |
| Occupancy and opacity masks | A fixed 4 KiB occupancy mask is maintained with the exact chunk content revision and copied into immutable meshing snapshots. Opacity remains render-table-dependent. | Prototype derived opacity/face masks keyed by content dependencies and render-table revision; measure total snapshot-plus-consumer cost. |
| Face culling and greedy meshing | Implemented with immutable neighborhood snapshots, material/render phases, bounded scheduling, stale rejection, buffer reuse, a reproducible isolated benchmark, occupancy-assisted empty/source rejection, surface-bound output reservation, and bounded invalidation-to-resident traces. | Prototype word-level face candidates; adopt slab or microbrick rebuilds only if measured edit P95 requires them. |
| Dynamic edit propagation | Dirty regions, neighbor dependencies, asynchronous mesh/light/collision work, upload quotas, exact mesh-stage latency tracking, and edit-burst coalescing telemetry exist. | Apply the 50 ms visual-response gate to clean rapid-edit runs; measure collision/light convergence and enforce explicit time budgets. |
| Streaming and persistence | Interest hysteresis, dirty pinning, deterministic generation, delta save/replication, residency budgets, and far clipmaps exist. Loading/generation and parts of save I/O remain synchronous. | Move disk/decode/generation/save stages off latency-critical threads and add journal durability, queue limits, and recovery tests. |
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
live format and does not yet justify replacing production storage. Render-table-dependent opacity,
macro validation, and compatibility tests for any eventually selected format remain open.

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
drain, mesh-work amplification, and benchmark schema v4 are also implemented. The sparse-cave and
checkerboard P95 gates are still missed, and the 50 ms rapid-edit macro gate still needs a clean
reference-machine run.

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
| Near time-to-visible | P95 at most 250 ms for resident or predicted inputs. |
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
