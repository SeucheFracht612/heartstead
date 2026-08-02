# Chunk streaming benchmarks

Status: generated and indexed saved-delta request-to-resident-publication gates pass on the
declared reference CPU. This is not yet a chunk-to-visible, save-under-load, physical-disk-cache,
lighting, collision, mesh, or GPU-upload closure.

The `heartstead_chunk_streaming_benchmark` executable drives the production
`ChunkLoadScheduler`, optional saved-delta source and decoder, deterministic terrain generator,
private chunk preparation, and owner-thread publication path. It retains per-chunk raw samples and
process/run provenance rather than reporting only a throughput average.

## Timing contract

Every coordinate in a required target ring becomes interesting at the same `steady_clock` instant.
Only four requests can enter the scheduler at once, but every later admission retains that original
interest timestamp. `interest_to_publication_us` therefore includes controller-side admission
deferral, worker queueing, optional delta-source read and decode, generation, preparation,
completed-result waiting, and successful world publication. It does not start a fresh clock when
capacity becomes available.

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
preparation, and total worker timings. For `saved_delta_publication`, `disk_read_ms` times an
immutable in-memory source lookup and record copy; it must not be interpreted as physical disk or
cold-cache latency.

## Workloads and invariants

- `near_load` declares a circular, uncached generated-chunk ring and records every successful
  resident publication.
- `teleport_recovery` fills the old-region request capacity, changes interest before owner
  publication, cancels every obsolete request, and then records the complete new ring from the
  teleport interest instant.
- `saved_delta_publication` materializes a flat history containing 16,384 unrelated edits plus one
  obsolete retained edit for every target, unloads those residencies, and loads one encoded saved
  edit per target through an immutable concurrently readable source. Publication must replace only
  that target's history, preserve the unrelated 16,384 records, and never rebuild the flat view.

All workloads fail closed on timeout, worker failure, stale output, duplicate publication,
off-interest publication, incomplete convergence, or a nonzero final working-memory reservation.
The teleport workload additionally requires every primed old-region request to retire as
cancelled. The saved-delta workload additionally requires the expected load source and edit count,
exact cell/history replacement, unchanged initial/final edit cardinality, preserved unrelated
history, and zero global-view rebuilds. These correctness and memory checks apply even when
performance gates are not enabled.

The default performance gates are:

| Metric | Default limit |
| --- | ---: |
| Generated near-ring interest-to-publication P95 | 250 ms |
| Teleport target-ring interest-to-publication P95 | 1,000 ms |
| Saved-delta interest-to-publication P95 | 250 ms |
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
  --output build/default-release/benchmarks/chunk-streaming-9ddfd9c-runN.json
```

Each process used two warmup runs and nine retained runs per workload. A radius of four contains 49
target chunks, producing 1,323 raw chunk samples per process across the three workloads. The saved
workload retained 16,384 unrelated edits, and the owner update cadence was 1 ms.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `9ddfd9cef43f3dd2b01d241619586275051f1f40` |
| Source state | clean tracked tree in every retained run |

Raw reports remain outside Git under `build/default-release/benchmarks/` as
`chunk-streaming-9ddfd9c-run{1,2,3}.json`.

## 2026-08-01 calibration

| Workload | Run 1 P95 | Run 2 P95 | Run 3 P95 | Median process P95 | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Near generated ring | 35.398 ms | 35.606 ms | 34.367 ms | 35.398 ms | 250 ms |
| Teleport target ring | 38.805 ms | 35.774 ms | 37.645 ms | 37.645 ms | 1,000 ms |
| Indexed saved-delta ring | 38.743 ms | 37.694 ms | 36.710 ms | 37.694 ms | 250 ms |

Across the three processes, the worst owner publication update was 55 us. Median process-level P95
scheduler pipeline latency was 4.274 ms near, 4.293 ms after teleport, and 4.314 ms with saved
deltas. Median process-level generation P95 was 1.955 ms, 1.953 ms, and 1.974 ms respectively. For
the saved workload, median process-level P95 source-read, decode, and private-prepare time was
0.001498 ms, 0.005208 ms, and 0.003795 ms.

Every process retained a 256 MiB reservation high-water mark, returned to zero, published all 49
target chunks per retained run, and passed every configured gate. Each teleport process retired 36
obsolete requests (four per retained run) and published none of them. Across 27 retained
saved-delta runs, all 1,323 publications began and ended with exactly 16,433 retained edits, with
zero stale/failed/rejected loads and zero flat-view rebuilds during publication.

CPU frequency, desktop workload, power policy, and thermal state were not controlled. These values
are a local calibration with clear headroom, not a portable hardware guarantee. Admission
deferrals are expected and visible in the report because the required ring is intentionally larger
than scheduler capacity.

## Boundary and remaining M5 work

The calibration proves generated block data and a one-edit saved delta can reach authoritative
resident publication through the bounded asynchronous path without making narrow publication
scale with unrelated edit history. Save and replication flushes use the same per-chunk index, while
full snapshot export still intentionally materializes the deterministic flat compatibility view.
The saved source is in memory, so this does not close physical-file read or cold-cache behavior.
The benchmark also stops before lighting, collision cooking, replication transport, client
residency, meshing, GPU upload, draw eligibility, and display. Calling this "time-to-visible" would
therefore be incorrect.

M5 still requires an end-to-end required-chunk visibility distribution, save-under-streaming and
large-snapshot-capture measurements, physical-disk/cache coverage, and explicit
lighting/collision/upload response gates. The general generated-world runtime controller also has
not yet adopted the loader; the live renderer-proof controller is currently the application path
that exercises it.
