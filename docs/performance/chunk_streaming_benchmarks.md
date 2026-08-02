# Chunk streaming benchmarks

Status: the generated-chunk request-to-resident-publication gate passes on the declared reference
CPU. This is not yet a chunk-to-visible, save-under-load, disk-cache, lighting, collision, mesh, or
GPU-upload closure.

The `heartstead_chunk_streaming_benchmark` executable drives the production
`ChunkLoadScheduler`, deterministic terrain generator, private chunk preparation, and owner-thread
publication path. It retains per-chunk raw samples and process/run provenance rather than reporting
only a throughput average.

## Timing contract

Every coordinate in a required target ring becomes interesting at the same `steady_clock` instant.
Only four requests can enter the scheduler at once, but every later admission retains that original
interest timestamp. `interest_to_publication_us` therefore includes controller-side admission
deferral, worker queueing, generation, preparation, completed-result waiting, and successful world
publication. It does not start a fresh clock when capacity becomes available.

This avoids the coordinated-omission shape in which a stalled system stops receiving measured
requests and consequently hides the missing latency samples. HdrHistogram's official API describes
the same failure mode and its expected-interval correction for periodic sampling. Heartstead does
not post-correct this workload because it records the fixed target population directly. The
[Google Benchmark guide](https://google.github.io/benchmark/user_guide.html) also recommends
wall-clock timing for code that uses internal worker threads and documents warmup and repeated-run
handling; the Heartstead harness follows those measurement choices without taking a dependency on
that library. See the
[HdrHistogram coordinated-omission API](https://hdrhistogram.github.io/HdrHistogram/JavaDoc/org/HdrHistogram/Histogram.html)
for the sampling rationale.

The scheduler separately reports `scheduler_pipeline_ms`, which starts when a request is actually
admitted. The difference between the two distributions exposes bounded admission pressure instead
of folding it into generation time. Raw samples also retain disk-read, decode, generation,
preparation, and total worker timings.

## Workloads and invariants

- `near_load` declares a circular, uncached generated-chunk ring and records every successful
  resident publication.
- `teleport_recovery` fills the old-region request capacity, changes interest before owner
  publication, cancels every obsolete request, and then records the complete new ring from the
  teleport interest instant.

Both workloads fail closed on timeout, worker failure, stale output, duplicate publication,
off-interest publication, incomplete convergence, or a nonzero final working-memory reservation.
The teleport workload additionally requires every primed old-region request to retire as
cancelled. These correctness and memory checks apply even when performance gates are not enabled.

The default performance gates are:

| Metric | Default limit |
| --- | ---: |
| Generated near-ring interest-to-publication P95 | 250 ms |
| Teleport target-ring interest-to-publication P95 | 1,000 ms |
| Maximum owner publication update | 500 us |

`--enforce-gates` evaluates these limits and returns exit code 3 after still writing the full report
when a limit is exceeded. Scheduler defaults remain independently bounded at two workers, four
active/completed requests, 64 MiB measured reservation per request, 256 MiB aggregate reservation,
two publications per owner update, and 500 us per update.

## Reproduction and provenance

The retained calibration was produced from a clean tracked tree with:

```text
cmake -S . -B build/default-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/default-release --target heartstead_chunk_streaming_benchmark -j2
build/default-release/apps/chunk_streaming_benchmark/heartstead_chunk_streaming_benchmark \
  --enforce-gates \
  --output build/default-release/benchmarks/chunk-streaming-6db534c-runN.json
```

Each process used two warmup runs and nine retained runs per workload. A radius of four contains 49
target chunks, producing 882 raw chunk samples per process. The owner update cadence was 1 ms.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `6db534c9a82a0a2b5233e8fd2f502f4fd2975fb9` |
| Source state | clean tracked tree in every retained run |

Raw reports remain outside Git under `build/default-release/benchmarks/` as
`chunk-streaming-6db534c-run{1,2,3}.json`.

## 2026-08-01 calibration

| Workload | Run 1 P95 | Run 2 P95 | Run 3 P95 | Median process P95 | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Near generated ring | 38.625 ms | 36.599 ms | 35.587 ms | 36.599 ms | 250 ms |
| Teleport target ring | 38.951 ms | 36.686 ms | 39.637 ms | 38.951 ms | 1,000 ms |

Across the three processes, the worst owner publication update was 77 us. The median process-level
P95 scheduler pipeline latency was 4.301 ms for the near workload and 4.298 ms after teleport. The
median process-level generation P95 was 1.952 ms and 1.965 ms respectively. Every process retained
a 256 MiB reservation high-water mark, returned to zero, published all 49 target chunks per run,
and passed both configured performance gates. Each teleport process retired 36 obsolete requests
(four per retained run) and published none of them.

CPU frequency, desktop workload, power policy, and thermal state were not controlled. These values
are a local calibration with clear headroom, not a portable hardware guarantee. Admission
deferrals are expected and visible in the report because the required ring is intentionally larger
than scheduler capacity.

## Boundary and remaining M5 work

The calibration proves generated block data can reach authoritative resident publication through
the bounded asynchronous path. It deliberately supplies no saved-delta source, so disk-read and
decode timings are zero. It also stops before lighting, collision cooking, replication transport,
client residency, meshing, GPU upload, draw eligibility, and display. Calling this
"time-to-visible" would therefore be incorrect.

M5 still requires an end-to-end required-chunk visibility distribution, save-under-streaming and
large-snapshot-capture measurements, explicit lighting/collision/upload response gates, and removal
of global edit-log copying from large saved-edit publication. The general generated-world runtime
controller also has not yet adopted the loader; the live renderer-proof controller is currently the
application path that exercises it.
