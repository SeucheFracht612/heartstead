# Performance budgets

Status: authoritative baseline, not a hardware guarantee.

Release benchmarks report median/p95/p99, 1%/0.1% lows, upload volume, memory, visibility, draws,
triangles, instances, lights, and per-pass GPU timing. Debug one-frame captures are regression checks,
not shipping performance numbers.

The renderer benchmark can enforce the research starting profiles with `--budget`:

| Profile | Interval | P95 | P99 | Maximum | Mean GPU when available | Upload burst | Rapid-edit P95 | Mesh builds/publication |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `compatibility` | 33.33 ms | 41.67 ms | 50.00 ms | 100.00 ms | 28.0 ms | 2 MiB/frame | 50 ms | 1.1 |
| `minimum` | 16.67 ms | 20.83 ms | 25.00 ms | 50.00 ms | 13.5 ms | 2 MiB/frame | 50 ms | 1.1 |
| `mainstream` | 11.11 ms | 13.89 ms | 16.67 ms | 33.33 ms | 9.0 ms | 4 MiB/frame | 50 ms | 1.1 |
| `high-end` | 8.33 ms | 10.42 ms | 12.50 ms | 25.00 ms | 6.7 ms | 4 MiB/frame | 50 ms | 1.1 |

Median frame time must also meet the profile interval. A headless run evaluates CPU frame pacing and
uploads but explicitly leaves the GPU check unevaluated. The edit limits apply only to the
sustainable `rapid-edits` workload and also require observed completions with no censored or
abandoned final work. They measure chunk mesh publication, not display scan-out, collision, or full
lighting convergence. These are renderer-workload gates, not yet complete end-to-end client, server,
memory, I/O, or network acceptance gates.

The chunk-streaming benchmark separately gates the authoritative generated-data path:

| Workload/metric | Default limit |
| --- | ---: |
| Near-ring interest to resident publication P95 | 250 ms |
| Teleport target-ring interest to resident publication P95 | 1,000 ms |
| In-memory saved-delta interest to resident publication P95 | 250 ms |
| File-backed saved-delta interest to resident publication P95 | 250 ms |
| File-backed payload-read P95 | 25 ms |
| File-backed generation-index open P95 | 100 ms |
| Owner-thread chunk publication update | 500 us |
| Save-under-streaming interest to resident publication P95 | 250 ms |
| Owner-side save submission | 0.25 ms |
| Save request to durable journal acceptance | 5,000 ms |
| Complete background save compaction | 75,000 ms |

These limits use wall-clock raw samples whose interest timestamp precedes bounded scheduler
admission. The saved-delta workload retains 16,384 unrelated edits and obsolete target histories,
while the physical workloads create a real 16,384-record `FileSaveDatabase` generation and use its
production indexed reader. Warm and Linux cache-drop-advice modes report index-open, payload-read,
and cache-treatment evidence separately. Accepted `POSIX_FADV_DONTNEED` is advisory and does not
establish a guaranteed cold-cache or physical-device miss. All limits stop at authoritative
block-data publication and do not claim lighting, collision, client replication, meshing, GPU
upload, draw eligibility, or display latency.

The save-under-streaming gates are opt-in because the current full generation publication is
proportional to the 16,384-record table. The benchmark submits one save at the same boundary as a
physical load ring, measures the entire owner handoff, measures request entry through stable journal
acceptance, and reports the worker encode/stable-write component separately. Snapshot cloning is
reported but not gated because the fixture already owns a `SaveSnapshot`; a production live-world
capture path still needs its own scale-qualified budget. Correctness gates require the old immutable
reader to remain valid through publication, destructive maintenance to fail fast while it is pinned,
and a quiescent null-source reader gap before stale-generation pruning and replacement-source
installation.
At the reference 16,384-record scale, three clean Release processes passed with median
save-under-streaming P95 of 38.225 ms, owner submission of 0.027907 ms, request-to-durable acceptance
of 16.730 ms, and complete compaction of 45.371 seconds. These are local regression evidence, not
portable hardware guarantees.

The chunk-delta journal benchmark separately gates production persistence work on the save worker:

| Metric | Default limit |
| --- | ---: |
| Initial writer open at 16,384 base records | 250 ms |
| Stable-storage chunk append P95 | 25 ms |
| Writer reopen with 136 journal entries P95 | 250 ms |
| Reader reopen with 136 journal entries P95 | 250 ms |
| Complete 16,384-record checkpoint | 75,000 ms |
| Post-checkpoint reader open | 250 ms |

The retained writer validates and accounts for the indexed base once; each timed append then
publishes one immutable checksummed entry. The stable-storage wait is deliberately included and must
not run on the gameplay thread. The checkpoint limit is a regression cap on current full-table
background work, not a latency SLO: reference checkpoint time is roughly 48 seconds and still
requires bounded runtime scheduling, retained-view rotation, and retry policy before live use. The
report also requires exact restart payload verification, journal teardown, and fixture cleanup. See
[Chunk delta journal benchmarks](../performance/chunk_delta_journal_benchmarks.md).

The chunk-render-readiness benchmark separately gates the production generated-data path through
exact current draw-command construction:

| Metric | Default limit |
| --- | ---: |
| Required interest to exact current draw command P95 | 250 ms |
| Owner-side upload preparation per update | 0.5 ms |
| Synchronous GPU fence wait | 0 ms |
| Mesh builds per published mesh | 2.5 |

Every target shares one pre-admission interest timestamp and must finish with a current mesh-stage
request, exact content/render/dependency revisions, a non-empty resident GPU-cache entry, and a
matching draw command. Headless runs validate host-side RHI ownership and draw construction;
explicit Vulkan runs additionally create physical buffers and submit copies without silently
falling back. Both endpoints precede GPU draw execution, presentation, and display scan-out.

The isolated voxel-response benchmark separately gates resident edit propagation:

| Metric | Default limit |
| --- | ---: |
| Exact collision-stage publication P95 | 100 ms |
| Complete resident-field relight convergence P95 | 250 ms |

Each paired add/remove edit begins only after the preceding collision and lighting work settles.
The first owner update runs immediately after the edit and later updates follow a 16,667 us cadence.
The report fails closed on a missing/censored sample, timeout, coalesced or abandoned measured
invalidation, failure, pending response, or non-current final stage. It records combined owner
update, collision cook/apply, relight solve/apply, copied-cell, and stale-work evidence. The
physics backend is explicit; retained calibration covers both the deterministic headless backend
and Jolt rather than treating one as an implicit substitute for the other.

These gates end at collision-body and complete light-field publication over an already resident
3x3 corpus. They do not claim burst-edit stability, larger-residency scaling, character contact,
replication, remeshing, upload, draw eligibility, or display response.

Transient latest-state replication has independent deterministic tick admission defaults:

| Metric | Global/tick | Per client/tick |
| --- | ---: | ---: |
| Snapshot messages | 512 | 128 |
| Encoded payload bytes | 256 KiB | 64 KiB |
| Serialization time | 4,000 us actual | 2,000 us attributed |

Movement and entity-motion payloads are encoded once per source and reused across recipients.
Global time is actual steady-clock codec time; each participating recipient is conservatively
charged the complete measured source cost for isolation, including a candidate later rejected by a
byte/message limit. Strict byte/message limits defer replaceable snapshots. The non-preemptible
codec call that crosses a global or client time boundary may complete, with its exact overshoot
reported and bounded to that one operation. Rotating clients, sources, and snapshot classes prevent
persistent low-identity preference. Reliable correctness traffic and tombstones are outside this
controller; the host's separate 256 KiB/client encoded-wire window still applies. These defaults are
implementation safety rails pending multi-client spread, convergence, impaired-network recovery,
P99, and soak calibration; they are not yet an M6 acceptance gate. See
[Networking architecture](networking.md#transient-tick-admission).

Reliable correctness traffic uses a separate encoded-wire backlog and drain envelope:

| Metric | Global hard cap | Per-client hard cap | Global/tick drain | Per-client/tick drain |
| --- | ---: | ---: | ---: | ---: |
| Messages | 8,192 | 1,024 | 512 | 128 |
| Encoded wire bytes | 64 MiB | 8 MiB | 1 MiB | 256 KiB |

All reliable application messages enter the FIFO before transport send. Drain service is rotating
round robin, and the 256 KiB/client one-second wire limit remains additive. A producer that has not
committed receives a hard admission failure at a full queue; host command-gateway overflow after
commit disconnects the affected client rather than losing its result or immediate event stream.
Other producers retain an explicit error for their owning resync/disconnect policy. Initial/final
backlog and exact byte movement are retained in tick telemetry. The deterministic four-message
stress test clears under a two-message/tick, one-message/client/tick profile in exactly two ticks.
These are bounded defaults, not yet calibrated multiplayer throughput or P99 gates. See
[Reliable application backlog](networking.md#reliable-application-backlog).

Chunk interest adds a second spatial bound above that FIFO. Defaults request a 39-chunk cylinder,
retain a wider 3-by-2 hysteresis volume, cap each client at 128 subscriptions, and transition at
most 4 additions/16 removals per ordinary update. Only loaded subscribed chunks are snapshot
candidates. A complete 32-slice client snapshot is admitted atomically against the exact reliable
message/byte envelope, and its encoded source payload is shared across recipients in the same tick.
Current/stale publications, shared serialization time, payload bytes, transition debt, snapshot
debt, and admission pressure remain visible. These are safety bounds pending the M6 multi-client
spread/traversal calibration, not final network SLOs.

High defaults include 16 visible terrain chunks horizontally with mesh/resident/load hysteresis,
8 MiB near and 8 MiB far-terrain uploads per frame, 512 MiB generic residency, 1,024 local lights,
32 lights per tile, two local shadow maps, 2048 directional shadow resolution, and 320 m shadow range.

New systems expose queue depth, dropped work, uploaded/resident bytes, visible/culled counts, and CPU/
GPU time. Exhaustion degrades deterministically through LOD, selection, or deferral rather than stalls
or unbounded allocation.

See [Renderer benchmarks](../performance/renderer_benchmarks.md) for workloads, timing semantics,
comparison rules, command usage, and dated measurements. See
[Chunk streaming benchmarks](../performance/chunk_streaming_benchmarks.md) for the open-loop
admission and resident-publication contract,
[Chunk delta journal benchmarks](../performance/chunk_delta_journal_benchmarks.md) for durable
per-chunk append, reopen, checkpoint, and restart-verification evidence,
[Chunk render-readiness benchmarks](../performance/chunk_render_readiness_benchmarks.md) for the
generated load-to-draw-command contract and physical-device boundary, and
[Voxel response benchmarks](../performance/voxel_response_benchmarks.md) for collision/relight
timing, invariants, tuning evidence, and dated headless/Jolt measurements. See the
[voxel optimization roadmap](../performance/voxel_optimization_roadmap.md) for the broader staged
budget system and calibration policy.
