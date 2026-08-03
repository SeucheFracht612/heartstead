# Voxel optimization roadmap

Status: maintained implementation plan derived from the optimization research brief and an audit
of the current engine on 2026-08-02.

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
| Permanent hierarchical profiling | Partial: retained CPU/GPU timers, counters, raw benchmark frames, and opt-in Tracy zones now cover major runtime, renderer, chunk, worker, lighting, collision, streaming, and process-simulation paths. | Extend zones and attribution as later stages are changed; add allocation ownership and queue-age plots. |
| Deterministic macrobenchmarks | Strong renderer catalog with representative and adversarial voxel, edit, streaming, lighting, fluid, particle, material, and environment scenes. The live renderer-proof test covers authoritative predictive interest, rapid teleports, cancellation, bounded eviction and deferred drain, required-set convergence, exact client replacement, and zero-reservation teardown. Separate open-loop chunk, save-under-streaming, chunk-delta journal, isolated voxel-response, end-to-end render-readiness, near/mid/far edit-transition, real-runtime multiplayer chunk-subscription, deterministic network-impairment, and process temporal-aggregation benchmarks retain request-to-resident, physical indexed-read, save handoff/acceptance/publication, stable-storage append, checkpoint, collision-publication, whole-field relight, upload, exact current draw-command, retained LOD convergence/continuity, multi-client spread/convergence/traversal, sustained disjoint hot edits, spatial exclusions, conditioned queue/private-memory soak, per-client wire, prediction/correction, eligible-unreliable loss, impairment depth, dense process parity, future-event backlog/lateness, modifier-work reduction, and server/system percentiles. The Vulkan runner also has a feature-gated, serialized presentation-completion workload with raw present IDs and waits. | Add guaranteed-cold and multi-filesystem, cold-start, broader burst edits, socket-backed shared-link impairment, correlated edit-to-GPU/presentation, physical-display, and workload-specific multi-hour endurance runs. |
| Reproducible provenance and gates | Renderer schema v4, chunk-streaming schema v4, chunk-delta-journal schema v1, voxel-response schema v1, chunk-render-readiness schema v1, multiplayer chunk-subscription schema v3, multiplayer network-impairment schema v2, and process temporal-aggregation schema v1 record source/build/CPU/device/run metadata, warmups, repetitions or raw ticks, workload configuration, and fail-closed lifecycle invariants. Renderer schema v4 also records presentation-timing request/support, raw validity/ID/wait, and valid-sample distributions. Optional gates cover frame distributions, uploads, available GPU, rapid-edit mesh response, generated/in-memory/file-backed saved resident publication, physical payload reads and index opens, save-under-streaming owner handoff/request-to-durable acceptance/full publication, durable append/reopen/checkpoint, exact collision publication, full-field relight convergence, required-chunk draw eligibility, synchronous GPU waits, mesh amplification, owner publication time, subscription bounds, relevance exclusions, codec reuse/overshoot, backlog recovery, hot-edit and soak P95/P99/max, wire volume, exact soak ownership/queues, precise private-memory slope/growth, aggregate and per-client impaired input/correction/bandwidth/depth, dense process parity/checksums, future-event backlog/lateness, resolver reduction, process P99/speedup, and general server P99. | Add historical relative-regression checks and the remaining guaranteed-cold/multi-filesystem I/O, correlated display, and socket-backed shared-link impairment gates. |
| Bounded jobs and cancellation | Generic and typed schedulers now bound pending/result work, expose backpressure and queue-age telemetry, age priorities, and support reasoned queued/cooperative cancellation. | Attribute per-type saturation in higher-level pipeline counters and tune limits from traces. |
| Versioned chunk pipeline | An owner-thread ledger now separates content, light, mesh, collision, persistence, and replication request/output revisions and states. Save/replication, mesh/GPU, collision/physics, and whole-field lighting publication are ticket-validated across edit and reload races. Palette-aware edits invalidate mesh, collision, lighting, and fluid consumers only when their dependency behavior changes. Strong-rollback command staging shallow-shares immutable dense cell fields and detaches the written chunks. | Calibrate stale-work amplification and latency under broader edit/streaming traces. |
| Compact voxel sections | Chunks remain fixed 32³ with contiguous dense `VoxelCell` production storage. Reproducible 16/32 experiments now cover dense, split, palette-packed, uniform-light, sparse-metadata, and adaptive split-dense fallback candidates. | Retain dense production storage while mask/macro work proceeds; add a medium-diversity crossover sweep before any storage selection. |
| Occupancy and opacity masks | A fixed 4 KiB occupancy mask follows the exact chunk content revision. Meshing snapshots also carry pooled greedy-cube and halo-padded full-occluder masks keyed by content dependencies and render-table revision. | Reuse the resident occupancy mask for later measured consumers; keep render-dependent masks derived and consumer-specific. |
| Face culling and greedy meshing | Implemented with immutable neighborhood snapshots, material/render phases, bounded scheduling, stale rejection, pooled buffers, reproducible isolated benchmarks, occupancy-assisted rejection, word-level face candidates/AO queries, surface-bound reservation, an isolated-cube culled fallback, and bounded invalidation-to-resident traces. | Keep slab or microbrick rebuilds deferred unless a future measured edit P95 again exceeds target. |
| Dynamic edit propagation | Dirty regions, dependency-selective neighbor invalidation, clipped fluid-region activation, asynchronous mesh/light/collision work, upload quotas, exact mesh/collision/relight lifecycle tracking, edit coalescing/abandonment telemetry, and calibrated visual, collision, relight, required-chunk upload-preparation, draw-eligibility, and multiplayer material-hot-edit gates exist. | Add broader burst-edit collision/relight amplification and correlate the existing GPU/presentation endpoints with edit and physical-display response. |
| Streaming and persistence | Interest hysteresis, dirty pinning, deterministic generation, indexed delta save/replication, residency budgets, and far clipmaps exist. Durable snapshot acceptance/compaction and application saves run through a bounded save worker. Failed-busy checkpoints now enter an application-owned, memory-reserved queue with capped exponential backoff, attempt/root limits, and explicit completion/exhaustion telemetry. A predictive owner-thread controller drives the bounded loader in the live renderer-proof stream, reserves demand capacity, tracks speculative outcomes, and emits capped eviction waves with deferred/overage telemetry. Saved-delta publication and narrow flushes no longer scan or copy global edit history, physical delta sources parse one base-plus-journal view per streaming epoch, and a retained writer publishes one checksummed file per update. Process-local readers/writers pin generation tables; append/publication mutations serialize across database instances; destructive maintenance fails fast for retry. The save-under-streaming harness proves pinned reads across full generation publication, an explicit reader gap, stale pruning, and future-source rotation. Warm/cache-advised reads and durable append/reopen/checkpoint gates pass at 16,384 records. | Add guaranteed-cold/multi-filesystem coverage, extend generator-backed loading beyond the renderer-proof world, and add scale-calibrated live save-capture gates. |
| Visibility, LOD, and GPU scaling | Frustum/distance/hierarchical visibility, HZB support, far clipmaps, indirect rendering, GPU arenas, upload staging, and pass timestamps already exist. | Tune only from captures; validate total culling benefit and retain broad fallback paths. |
| Simulation and multiplayer scale | Simulation LOD, server authority, fixed-step runtime, bounded reliable/transient replication, player-centered hysteretic chunk subscriptions, and bounded timestamp-process future events now run in `ServerRuntime`. Automatic process transitions publish matching reliable events and typed deltas under one host sequence. Chunk snapshots are relevance-limited, identity/revision tracked, atomically queued, globally codec-time-bounded, and encoded once across recipients. Committed voxel events and typed deltas use the same exact-published-chunk relevance rule. Contiguous delivered voxel deltas advance the recipient publication and suppress redundant full snapshots; server gaps retain the full-snapshot fallback. Client intake applies exact-next revisions, ignores deltas covered by a newer complete snapshot, and rejects gaps. Clean eight-client spread/convergence/traversal, 120-tick hot-edit, conditioned 64-cycle queue/private-memory soak, deterministic 600-tick 100 ms RTT / 2% unreliable-loss, and 65,536-process dense-reference runs pass server/process P99, relevance, sharing, wire, exact apply, backlog/lateness, semantic/checksum parity, logical ownership, precise memory, work-reduction, prediction, correction, bandwidth, impairment-depth, and transport-integrity gates. | Add crop/population/economy aggregate models, socket-backed shared-link impairment, and workload-specific multi-hour endurance. |

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
rejects cancelled/stale work before publication. The renderer-proof runtime now drives this path
through the predictive owner controller. Its rapid-teleport test caps eviction at three chunks per
update, observes deferred work, proves cancellation and bounded drain, recovers all 441 required
chunks plus only a budgeted speculative fringe, replaces an evicted/reloaded chunk by exact remote
identity and revision on the client, and returns pending work and reservations to zero. The focused
save/load and response paths pass warning-as-error builds plus ASan/UBSan and TSAN.

The file-backed saved-delta source now opens and validates one generation-scoped sorted index before
workers use it. Concurrent reads perform `O(log n)` lookup and one payload-file read; they no longer
parse and sort `index.txt` for every requested coordinate. Existing readers remain pinned across a
new generation activation and later journal appends, legacy inline snapshots remain supported, and
corrupt indexes/journal entries fail at open.

The streamed write path now retains a generation-scoped writer and publishes each update as one
bounded, immutable, versioned, checksummed file with a stable-storage acceptance boundary. Readers
overlay the latest sequence per coordinate; restart, repeated-key ordering, pinned end marks,
generation rollover, pending full-snapshot authority, interrupted temporary cleanup, checkpoint
ordering, legacy conversion, and corruption failure have focused coverage. Checkpoint durably
materializes the effective base before removing the covered journal. Database instances for one
normalized root now share process-local mutation serialization and generation-table leases;
destructive maintenance returns `save_database.busy` while a retained reader or writer is live.
Cross-process exclusion remains open. Application checkpoint failures caused by a busy mutation
boundary now retry without appending another snapshot: the low-priority worker request carries the
original memory reservation, and the owner queue uses 250 ms to 30 second capped backoff, eight
attempts, and an eight-root limit. Exhaustion and non-retryable storage failures remain explicit.

Schema v4 adds an opt-in save-under-streaming phase around the production scheduler and database. One
physical reader stays pinned while a one-worker save is submitted at the same boundary as an open-loop
load ring. The report gates owner submission, request-to-stable-journal acceptance, full background
generation publication, concurrent chunk P95, payload reads, reader open, and owner publication. It
then requires destructive maintenance to return busy, pauses new submissions, installs a null-source
reader gap, prunes the stale generation, and rotates future submissions to a newly opened reader.
Fixture snapshot cloning is reported but not gated because it is not a live-world capture path.
Three clean 16,384-record Release processes passed every gate with median concurrent-load P95 of
38.225 ms, owner save submission of 0.027907 ms, request-to-durable acceptance of 16.730 ms, and
complete compaction of 45.371 seconds. Every process retained all 49 pinned deltas, returned load
reservations to zero, observed busy maintenance, pruned during the reader gap, verified all payloads
through the replacement reader, and recorded exactly two rotations. See
[Chunk streaming benchmarks](chunk_streaming_benchmarks.md#schema-v4-save-under-streaming-calibration).

The schema-v1 physical journal benchmark retains individual append samples, restart-style writer
and reader opens, complete checkpoint, exact post-restart payload verification, provenance, and
cleanup. Three clean 16,384-record Release processes measured median process-level append P95 at
3.336 ms, writer/reader reopen P95 at 21.732/13.883 ms, and complete checkpoint at 48.254 seconds.
A 128/4,096/16,384 sweep held append P95 essentially flat at 3.247/3.345/3.336 ms while checkpoint
grew from 0.388 to 12.088 to 48.254 seconds. This closes full-table rewrite in each streamed
foreground update but confirms that checkpoint remains proportional to the complete table. See
[Chunk delta journal benchmarks](chunk_delta_journal_benchmarks.md).

The open-loop chunk benchmark declares every target coordinate at one instant so bounded admission
delay remains in each raw sample. Schema v3 added a real 16,384-record `FileSaveDatabase` generation,
separate one-time index-open and per-payload-read timings, an explicitly primed workload, and a Linux
workload that records whether cache-drop advice was accepted. On the declared reference CPU, three
clean Release processes measured median process-level P95 at 37.694 ms near, 40.755 ms after
teleport, 38.596 ms for the in-memory delta source, 37.544 ms for warm physical deltas, and
42.921 ms after accepted cache-drop advice. Median warm/advice-accepted payload-read P95 was
0.042166/0.238455 ms and index-open P95 was 12.808863/15.957277 ms. The worst owner publication
update was 126 us. Correctness gates require complete convergence, exact obsolete cancellation, no
off-interest/stale/failed publication, exact saved-history replacement, evidence for the requested
cache treatment, successful fixture cleanup, zero global-view rebuilds, and zero final reservation.
Accepted `POSIX_FADV_DONTNEED` remains advisory rather than proof of a cold device read. See
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

A small release lifecycle sample measured save owner handoff at 0.046 ms, and the full 16,384-record
save-under-streaming calibration measured a 0.027907 ms median process value, both below the 0.25 ms
target. Schema v4 separates owner handoff, request-to-durable acceptance, worker durable operation,
and full compaction while exercising reader rollover. This is still not complete live-capture
closure: snapshot capture scales with owned world state and the benchmark fixture already owns the
typed snapshot.
The original physical fixture's production writer took a median 48.324 seconds to create 16,384
durable per-chunk records; the new calibration measures the same full-table shape at 48.047 seconds
for generation write and 48.254 seconds for checkpoint while keeping individual durable streamed
appends near 3.3 ms P95. Generator hookup for worlds beyond the renderer-proof stream,
guaranteed-cold and multi-filesystem coverage,
burst-edit/large-residency response,
large live-snapshot-capture benchmarks, and correlated required-chunk GPU execution,
presentation-completion, and physical-display timing remain before M5 can be marked complete.

### M6 — world and multiplayer scale

Deliverables:

- measured predictive streaming with hit/waste/cancellation statistics;
- retention hysteresis driven by memory pressure and explicit eviction value;
- near/mid/far representation transition tests after edits;
- simulation temporal-LOD, future-event process execution, catch-up budgets, and dense-reference
  scale gates;
- per-client/global replication byte and serialization-time quotas;
- multiplayer spread, convergence, rapid traversal, and long-soak benchmarks.

Exit gate: server P99 meets its tick interval, bounded stress backlog clears within two ticks,
streaming queues respect visible-hole deadlines, and memory has no upward slope after caches reach
their caps.

Implementation status (in progress): the streaming policy now exposes the complete immediate
desired set and layers a stateful predictive planner over it. Each viewer contributes both velocity
and view direction to a bounded, deduplicated trajectory of shifted future-demand footprints. This
keeps prediction useful when the immediate radius is wider than the center's travel distance.
Required loads remain separate from speculation; speculative submissions and active work have
independent hard caps, elevated pressure halves new speculative admission, and critical pressure
disables it. Reversal, expiry, and teleport paths issue explicit cancellation requests, while a
cancellation that loses the race to publication becomes an immediate low-value eviction candidate.

The retained telemetry deliberately separates prediction accuracy, timely coverage, prefetch-to-use
lead time, late hits, waste, requested/actual cancellation, cancellation misses, failure, and stale
publication. This follows the accuracy/coverage/timeliness distinction used by the USENIX ATC 2020
[Leap prefetcher](https://www.usenix.org/system/files/atc20-maruf.pdf) and the FAST 2007 observation
that speculative data must not displace more valuable demand data when accuracy is low
([AMP prefetching rationale](https://www.usenix.org/legacy/events/fast07/tech/full_papers/gill/gill_html/node3.html)).
The policy does not infer success from hit rate alone.

Every clean non-required resident chunk also receives an inspectable eviction value composed from
estimated reload cost, pressure-scaled spatial retention, decaying temporal retention, speculative
pollution penalty, and viewer distance. Low values leave first; pressure may override hysteresis,
but required or persistence/replication-dirty chunks remain pinned. Owner destruction is capped per
update; deferred work, projected post-wave overage, and overage that no eligible clean candidate can
resolve are reported separately. The cost/locality structure is informed by
[GreedyDual-Size](https://www.usenix.org/legacy/publications/library/proceedings/usits97/full_papers/cao/cao_html/node8.html),
without claiming its cache-optimality result for voxel residency. Pressure is an explicit portable
input; Linux hosts may eventually feed it from cgroup or system
[PSI thresholds](https://cdn.kernel.org/doc/html/latest/accounting/psi.html), but the engine policy
does not depend on `/proc`.

Focused tests cover directional and camera prediction, shifted future-demand footprints,
multi-viewer deduplication, timely demand conversion, reversal, teleport, cancellation races,
dirty pinning, temporal retention, pressure overrides, bounded/deferred eviction waves, unresolved
caps, and invalid inputs. An exclusive owner-thread controller drives the production scheduler,
reserves demand capacity, publishes outcomes back into the policy, and applies the resulting clean
evictions. The paired
[predictive streaming benchmark](predictive_streaming_benchmarks.md) compares that path against a
no-prefetch baseline while retaining real hit/waste/cancellation, visible-hole, owner-publication,
and memory-slope evidence. Three clean Release processes passed every gate with identical behavioral
results: prediction reduced visible-hole steps from 59/67 to 30/67, raised immediate residency from
11.94% to 55.22%, resolved at 82.61% accuracy with 85.07% timely coverage, completed all four
cancellation requests, and held the late-soak residency slope at zero. Median baseline/predictive
visible-hole P95 was 6.535/5.658 ms and median worst owner publication was 46 us. See the
[clean reference calibration](predictive_streaming_benchmarks.md#clean-reference-calibration).

The live streaming-enabled renderer-proof server now derives deterministic viewer motion and view
direction from authoritative player state, detects discontinuities, and feeds that controller every
simulation tick. Tracy plots, the runtime overlay, and inspection output expose required residency,
pending/speculative work, prediction accuracy/coverage, deferred eviction, and projected/unresolved
overage. Its integration test forces a teleport under a three-eviction wave cap, observes the
backlog, returns, and proves all 441 required chunks recover, residency stays at or below target,
server/client counts and exact remote identities converge, and pending/deferred/reserved work
reaches zero. This accepts the live predictive-streaming slice; generator hookup for other world
types remains open.

Temporal simulation admission is now a separate deterministic layer over raw LOD classification.
Every subject carries a positive estimated work cost; each tick has hard subject-count, work-unit,
per-LOD catch-up, and aggregate catch-up limits. Due work is ordered by oldest deadline with stable
identity tie-breaking, duplicate identities and permanently unserviceable costs fail planning, and
full-detail catch-up remains fixed-step by default. Scheduled work returns an exact commit timestamp
while all remaining time debt, maximum lateness, saturation, and exhausted-budget state stay
inspectable. Focused stress coverage proves a four-subject burst under a two-update budget clears in
two planning ticks without losing elapsed time. See
[Simulation LOD architecture](../architecture/simulation_lod.md#catch-up-commit-contract).

Timestamp-based production processes now provide the first concrete runtime aggregate model. A
bounded future-event controller runs after authoritative clock advancement, resolves environmental
modifiers only at admission/events, caps stalled reevaluation and catch-up, and returns exact
transition telemetry. Commands that may change derived room/power/assembly dependencies reset it
conservatively. Automatic transitions publish matching reliable events and typed deltas under one
host replication sequence. The schema-v1
[process temporal-aggregation benchmark](process_temporal_aggregation_benchmarks.md) pairs 65,536
processes with a dense reference, retains 3,000 logical ticks, and gates semantic/checksum parity,
two-tick burst backlog/lateness, process P99, median speedup, and resolver reduction. Three clean
Release processes measured 0.140991 ms median P99, 17.725861x speedup, 99.6484% fewer resolver
calls, and zero correctness or budget failures. This accepts the timestamp-process temporal-LOD
slice. Crop, animal, population, economy, and settlement aggregate models still require their own
state contracts and scale evidence.

Transient movement and entity-motion replication now passes through one deterministic global and
per-client admission controller. Message and encoded-payload ceilings are strict; actual shared
codec time has a global boundary with explicit one-operation overshoot, and every participating
client receives a conservative attributed-time charge even if byte/message admission rejects the
candidate. Identical source state is encoded once and reused across
recipients, removing the previous size-probe plus message re-encode and changing codec work from
source-by-client to source-only. Recipient, source, and snapshot-class cursors rotate independently,
and fail-closed telemetry reproduces global totals from identity-sorted client reports. Focused
runtime coverage proves two clients each receive one snapshot under a one-message client quota while
only one codec operation runs. The policy is grounded in the shared-list scaling rationale of
Epic's official
[Replication Graph documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph-in-unreal-engine),
the flow-isolation motivation of [RFC 8290](https://www.rfc-editor.org/rfc/rfc8290), and the
separate transport-control boundary in [RFC 9002](https://www.rfc-editor.org/rfc/rfc9002).

Reliable correctness traffic now enters one exact-wire-size FIFO layer with hard global and
per-client message/byte caps. Rotating round-robin delivery enforces global and per-client tick
message/byte limits across the command and post-command replication phases; a failed send blocks
only its client for that delivery cycle. Direct queue exhaustion is explicit, while command results
and immediate event replication that cannot be queued after authoritative commit disconnect the
affected client rather than silently losing output. Later producers retain an explicit error for
their owning resync/disconnect policy. Initial/final backlog, attempted/delivered bytes, tick/window
deferrals, failures, and overload identities are inspectable. Deterministic coverage proves healthy
command progress beside a failing peer, strict FIFO ordering, equal service for two constrained
clients, and zero backlog on the second recovery tick. This follows the bounded-buffer rationale of
[RFC 9000](https://www.rfc-editor.org/rfc/rfc9000#section-4) and the stalled-association isolation
warning in [RFC 6458](https://www.rfc-editor.org/rfc/rfc6458#section-3.1.2), without claiming a
complete congestion controller.

A bounded chunk-subscription planner validates per-client caps and transition quotas, selects
nearest desired additions plus farthest removals with retain hysteresis, and evicts non-desired
retained chunks under cap pressure so current interest cannot starve. `ServerRuntime` now persists
that state per player, derives centers from authoritative positions, scans only loaded subscribed
chunks, and compares load identity/content revision against each client's publication table.
Reliable unsubscriptions precede server-side publication removal. Whole 32-slice snapshots use
atomic exact-wire backlog admission, and one per-tick encoded snapshot is reused across all matching
recipients. Collision-interest snapshots precede the player prediction seed; state remains pending
and retryable if a small queue cannot accept both.

Focused coverage now spans planner extremes and cap pressure, FIFO removal/resubscription, unrelated
far-chunk exclusion, bounded teleport convergence, two-client shared encoding, reliable-backlog and
codec-time deferral/recovery without partial publication, and collision-first bootstrap under a
64-message cap. Ordinary ticks defer new cache misses at a 4,000 us global codec boundary while
retaining exact one-operation overshoot; direct in-memory bootstrap remains synchronous.

The schema-v3
[multiplayer chunk-subscription benchmark](multiplayer_chunk_subscription_benchmarks.md) retains the
schema-v2 foreground workload: eight real clients traverse a shared cluster, disjoint spread, six
rapid transitions, and steady state, plus 120 ticks where every client commits one material-only
voxel edit in its own non-overlapping region. The older schema-v1 clean reference retains
2.505/4.747 ms median overall P95/P99 for the snapshot-only workload. Three clean schema-v2 Release
processes at commit
`b2d4bde` passed every gate with median overall P95/P99 of 0.441/2.569 ms and median hot-edit
P95/P99/max of 0.424/0.461/0.616 ms. Peak hot traffic was 860 bytes/client/tick. All 960 edits
applied exactly, all 6,720 cross-region deliveries were excluded, 960 contiguous publications
advanced, 960 full snapshots were avoided, and zero publication gaps occurred.

After mixed snapshot/delta ordering was closed, three further clean Release processes at commit
`5cd11bb` retained every behavioral and wire invariant with median overall P95/P99/max of
0.390/2.482/4.752 ms and hot-edit P95/P99/max of 0.381/0.409/0.504 ms. This confirms that tentative
client revision cursors and superseded-delta filtering did not consume the hot-path margin.

Raw named-system timing identified command transaction staging, not replication or fluids, as the
initial bottleneck: the gateway averaged 20.271 ms when every command deep-copied all resident 32³
cell arrays. Dense cell fields now use detach-before-write sharing across staged `WorldState`
copies, retaining strong rollback while making copy cost proportional to the written chunks. A
controlled diagnostic reduced gateway average to 0.306 ms; the three clean accepted runs measured a
0.296 ms median. Palette behavior keys make clay/stone mesh-only, preserve derived light, and avoid
collision/fluid work. Fluid activation now walks only the exact clipped dirty intersection.

Committed voxel event batches and typed deltas share an owner-only chunk routing scope derived from
exact-current client publications. A delivered, recipient-visible edit advances only a complete
identity-matched preceding publication; a missing identity or revision remains a measured gap and
falls back to the ordinary complete snapshot path. Focused two-client coverage proves one near
delivery, one far exclusion, absence of both payload types at the far client, delta-based near
publication advancement, and exact late-interest recovery by a full snapshot. Client-side ordering
now accepts an exact-next delta from a snapshot base, prevents an older delta from regressing a
newer snapshot, sequences repeated same-cell edits, and fails closed on a gap without advancing its
cursor.

Schema v3 then conditions bounded command histories with 256 alternating edit ticks and allocator
reuse with eight unmeasured cycles before a 64-cycle measured continuation. Each cycle traverses
all clients from their hot regions to disjoint spread regions and back, then applies two edits that
restore the persistent voxel state. The report preallocates compact tick timing and cycle-endpoint
resource samples before its baseline, and gates latency, exact apply/exclusion totals, convergence,
backlog recovery, logical ownership, settled queues, thread/open-file growth, and precise private
resident-memory slope/growth. Three clean Release processes at commit `1b1db1b` each retained 1,408
soak ticks, 1,024/1,024 exact client states, 7,168/7,168 exclusions, and 64 backlog bursts recovered
within two ticks. Median soak P50/P95/P99/max was 0.062/2.686/4.734/4.841 ms. All runs held
baseline/peak/final ownership at 76 server chunks, zero server edit records, eight total client
chunks, and 2,144 client-owned record units; precise private resident-memory endpoint growth and
descriptive OLS slope were zero. This accepts the deterministic multi-client queue/private-memory
soak slice, while explicitly not claiming multi-hour, impaired-network, allocator-attributed, or
statistically significant lifetime stability.

The schema-v2
[multiplayer network-impairment benchmark](multiplayer_network_impairment_benchmarks.md) now drives
eight production clients through 600 measured 60 Hz ticks at 100 ms nominal RTT, uniform
plus-or-minus 10 ms configured delay variation, and 2% unreliable loss. Aggregate tick rows and
sorted per-client rows exactly reconcile prediction, correction, traffic, loss-eligible, drop,
connection, and impairment-depth evidence. Measured timing and traffic stay separate from a bounded
recovery interval that creates no new prediction input and requires exact sequence and state
convergence.

Three clean Release processes at commit `32463a6` passed every aggregate and per-client gate. The
median server P95/P99/max was 0.184/0.190/0.209 ms; 4,781-4,782 of 4,800 measured inputs were
accepted, every client reached sequence 600 with an empty prediction buffer, and no hard correction
occurred. Aggregate server-to-client offered load averaged 1,347,076.5-1,357,186.6 bytes/s; the
largest per-client average/rolling-second values were 169,652.5/181,795 bytes. The legitimate
asynchronous chunk-publication branch peaked at 682 aggregate and 88 per-client impaired messages,
inside the 1,024/128 caps. The deterministic in-process multi-client impairment slice is accepted.
The timestamp-process temporal aggregation slice is accepted separately above; socket-backed
shared-link and multi-hour impairment remain separate validation work.

The schema-v1
[terrain edit-transition benchmark](terrain_edit_transition_benchmarks.md) now starts from complete
near/mid/far residency, applies one grid-aligned authoritative surface edit at a deterministically
selected LOD boundary, and retains exact-current near draw, complete mid replacement, complete far
replacement, owner cost, upload, queue, memory-continuity, stale-work, and teardown evidence. A
second forced edit race must coalesce near work and reject stale far tickets without dropping any
resident draw. Three clean Release headless processes measured process-level near/mid/far/full P95
at 17.176–17.248 ms; three Intel Graphics Vulkan processes measured 21.754–24.882 ms. Worst owner
updates were 1.521/9.189 ms, upload preparation stayed below 0.016 ms, synchronous GPU wait stayed
zero, and pipeline occupancy peaked at two of three. All six processes passed the calibrated
50/250/500/500 ms response, 12 ms owner, 0.5 ms upload-preparation, zero-wait, bounded-work,
continuity, supersession, and resource-teardown gates. This implements the isolated retained
near/mid/far transition slice. Burst and broad invalidations, correlated GPU
execution/presentation/display response, and multi-cycle near/mid/far transition-soak evidence
remain open.

### M7 — trace-gated GPU work

Candidates include further GPU-driven visibility, meshlets, descriptor indexing, compute meshing,
virtualized resources, and sparse far-field structures. Each candidate requires a captured CPU,
GPU, bandwidth, or residency bottleneck; shipping-hardware measurements; correctness and fallback
parity; and a net macrobenchmark improvement. A candidate without that evidence is rejected or
deferred.

The first candidate has now passed through that decision process. A bounded near-terrain MDI
prototype collapsed 902 compatible mountains draws to 12 calls and 977 flat draws to four calls.
On Intel Graphics (LNL), validation-off Release recording fell 45-46%, but mean CPU/GPU time rose
24/45% on mountains and 38/64% on flat; mountains P95 rose 39% and flat P95 doubled. Direct Vulkan
recording in the production configuration was only 0.51-0.69 ms, while the earlier 4-6 ms signal
was validation-instrumented driver work. The prototype was therefore removed rather than hidden
behind a default-off production path. Raw samples and the complete configuration are retained in
[Renderer benchmarks](renderer_benchmarks.md#near-terrain-multi-draw-indirect-rejection--2026-08-02).

The standards audit did identify two independent correctness requirements worth retaining:
`multiDrawIndirect` does not imply `drawIndirectFirstInstance`, and every MDI call must stay within
`maxDrawIndirectCount`. The renderer now enables and gates the former explicitly, reports the
physical-device limit through the RHI, splits far-terrain groups at that limit, and falls back to
direct draws when the limit cannot produce a multi-draw call. No capture currently justifies
meshlets, compute meshing, descriptor indexing, virtualized resources, or sparse far-field
structures. Actual GPU execution remains represented by asynchronous timestamps.

An explicit presentation-completion endpoint is now available for diagnostics. When requested, the
Vulkan device selection and feature chain require `VK_KHR_present_id` plus
`VK_KHR_present_wait`; each present receives a monotonic ID, waits with a finite timeout, and
publishes validity/ID/wait telemetry through the RHI and renderer schema v4. Unsupported requests
fail closed, while ordinary rendering does not enable the extensions, sample a clock, or wait. The
mode intentionally serializes presentation and therefore cannot be used as a normal frame-throughput
configuration.

Three clean 600-frame Release processes per scene on Intel Graphics (LNL) retained 3,600/3,600
valid samples and contiguous measured IDs. Flat presentation-wait medians/P95s ranged
3.357-3.757/6.304-6.823 ms; mountains ranged 8.984-9.111/11.403-11.844 ms. A validation-enabled
Debug smoke completed 30/30 waits without validation messages. See
[Renderer benchmarks](renderer_benchmarks.md#vulkan-presentation-completion-calibration--2026-08-02).
This closes generic host-observed queue-call-to-presentation-completion instrumentation, not
edit-to-present correlation, a precise presentation timestamp, compositor-to-panel scan-out, or
input-to-photon latency. Those still require a separate acceptance workload and, for physical
display response, an external or platform-specific endpoint.

## Initial acceptance matrix

| Measure | Starting gate |
| --- | --- |
| Client frame pacing | Tier median; P95 at most 1.25 times interval; P99 at most 1.5 times; no unexplained frame above 3 times interval. |
| Minimum/mainstream/high-end GPU | Mean at most 13.5/9.0/6.7 ms when timestamps are available. |
| Upload burst | At most 2 MiB/frame for minimum and compatibility; 4 MiB/frame for mainstream and high-end. |
| Server tick | P99 at most the tick interval; no sustained backlog. |
| Timestamp-process scale | Zero dense-reference/checksum/budget failures; backlog and lateness at most two ticks; P99 at most 5 ms; median speedup at least 5x. |
| Multiplayer material hot edit | P95 at most 12.5 ms, P99 at most 16.667 ms, maximum at most 50 ms, and exact wire at most 2 KiB/client/tick. |
| Mesh latency | Representative P95 at most 4 ms; adversarial P95 at most 10 ms. |
| Local edit | Visual P95 at most 50 ms; adjacent collision at most 100 ms. |
| Lighting | Begins within one frame; ordinary convergence P95 at most 250 ms. |
| Near draw eligibility | P95 at most 250 ms for resident or predicted inputs; GPU execution and presentation must be correlated separately. |
| Presentation diagnostic | A requested supported run has valid, strictly increasing present IDs for every ordinary measured frame; wait distributions are descriptive, not normal frame budgets. |
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
