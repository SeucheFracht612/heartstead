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
| Saved-delta interest to resident publication P95 | 250 ms |
| Owner-thread chunk publication update | 500 us |

These limits use wall-clock raw samples whose interest timestamp precedes bounded scheduler
admission. The saved-delta workload retains 16,384 unrelated edits and obsolete target histories,
but uses an immutable in-memory delta source to isolate decode, private preparation, and indexed
owner publication from physical filesystem variability. All limits stop at authoritative
block-data publication and do not claim lighting, collision, client replication, meshing, GPU
upload, draw eligibility, or display latency.

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

High defaults include 16 visible terrain chunks horizontally with mesh/resident/load hysteresis,
8 MiB near and 8 MiB far-terrain uploads per frame, 512 MiB generic residency, 1,024 local lights,
32 lights per tile, two local shadow maps, 2048 directional shadow resolution, and 320 m shadow range.

New systems expose queue depth, dropped work, uploaded/resident bytes, visible/culled counts, and CPU/
GPU time. Exhaustion degrades deterministically through LOD, selection, or deferral rather than stalls
or unbounded allocation.

See [Renderer benchmarks](../performance/renderer_benchmarks.md) for workloads, timing semantics,
comparison rules, command usage, and dated measurements. See
[Chunk streaming benchmarks](../performance/chunk_streaming_benchmarks.md) for the open-loop
admission and resident-publication contract, and
[Voxel response benchmarks](../performance/voxel_response_benchmarks.md) for collision/relight
timing, invariants, tuning evidence, and dated headless/Jolt measurements. See the
[voxel optimization roadmap](../performance/voxel_optimization_roadmap.md) for the broader staged
budget system and calibration policy.
