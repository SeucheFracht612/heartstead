# Chunk streaming benchmarks

Status: generated, in-memory saved-delta, physical file-backed saved-delta, and opt-in
save-under-streaming request-to-resident-publication gates pass at 16,384 records on the declared
reference CPU. The file-backed modes
include a primed-cache state and a Linux run for which cache-drop advice was accepted. The concurrent
save phase also proves pinned-generation continuity, bounded owner handoff, durable acceptance, full
generation publication, fail-fast destructive maintenance, and explicit reader rotation. It does
not claim guaranteed cold-cache behavior, production world-capture cost, or visibility/display
closure.

The `heartstead_chunk_streaming_benchmark` executable drives the production
`ChunkLoadScheduler`, optional saved-delta source and decoder, deterministic terrain generator,
private chunk preparation, and owner-thread publication path. Schema v4 retains per-chunk raw
samples, physical-fixture metadata, cache-treatment evidence, the optional save/rotation phase, and
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
of folding it into generation time. Raw samples retain source-read, decode, generation,
preparation, and total worker timings:

- For `saved_delta_publication`, `disk_read_ms` is an immutable in-memory lookup and record copy. It
  must not be interpreted as filesystem latency.
- For both `file_delta_*` workloads, `disk_read_ms` times the production indexed reader's payload
  file operation. Index parsing and validation occur once in `delta_reader_open_ms`, before the
  ring's interest timestamp, so the report exposes that cost separately instead of charging it to
  an arbitrary chunk.
- Physical-fixture creation is outside every retained workload sample. `physical_fixture.setup_ms`
  reports the complete production save-generation write, and `encoded_payload_bytes` reports the
  payload bytes written.

With `--save-under-streaming`, a fresh one-worker `SaveScheduler` receives a complete fixture
snapshot immediately after the initial target-load admissions. `save_submission_ms` times the
complete owner-side `submit` call, including request validation, memory estimation, and bounded job
handoff. `save_durable_acceptance_ms` begins at entry to that call and ends when the checksummed
journal record reaches the platform stable-storage boundary; `save_durable_operation_ms` retains the
worker's encode-and-stable-write component separately. `save_compaction_ms` measures background full
generation publication. The fixture snapshot clone happens before interest declaration and is
reported as `snapshot_clone_ms`, but it is deliberately not gated: copying a benchmark-owned
snapshot is not evidence for the cost of capturing a live production world.

## Workloads and invariants

- `near_load` declares a circular, uncached generated-chunk ring and records every successful
  resident publication.
- `teleport_recovery` fills the old-region request capacity, changes interest before owner
  publication, cancels every obsolete request, and then records the complete new ring from the
  teleport interest instant.
- `saved_delta_publication` materializes a flat history containing 16,384 unrelated edits plus one
  obsolete retained edit for every target, unloads those residencies, and loads one encoded saved
  edit per target through an immutable concurrently readable source.
- `file_delta_warm` writes a real indexed `FileSaveDatabase` generation, primes every target
  payload through one reader, opens a fresh reader, and loads the target ring through the production
  `FileSaveChunkEditDeltaSource` adapter.
- `file_delta_drop_cache_advised` is Linux-only. Before each measured ring it requests
  `POSIX_FADV_DONTNEED` for `current.txt`, the active `index.txt`, and every target payload, then
  requires every open, advice, and close operation to succeed. The report records attempted and
  accepted file counts. The call is advisory: the kernel may retain pages, partial pages are not
  discarded, and acceptance is not evidence of a cold device read. See
  [`posix_fadvise(2)`](https://man7.org/linux/man-pages/man2/posix_fadvise.2.html).
- The opt-in `save_under_streaming` phase opens and pins the current physical generation, starts one
  open-loop file-backed load ring and one full background save at the same interest boundary, and
  requires every load to publish the pinned generation's exact delta. After the save publishes a new
  immutable generation, pruning must return `save_database.busy` while the old view is retained. The
  harness then pauses submissions, installs a null source to create an explicit reader gap, prunes
  the stale generation, opens the new generation, and rotates future submissions to it.

All workloads fail closed on timeout, worker failure, stale output, duplicate publication,
off-interest publication, incomplete convergence, or a nonzero final working-memory reservation.
The teleport workload additionally requires every primed old-region request to retire as
cancelled. Every saved-delta workload requires the expected load source and edit count, exact
cell/history replacement, unchanged initial/final edit cardinality, preserved unrelated history,
and zero global-view rebuilds.

Physical workloads additionally require the requested indexed record count and active generation,
the exact cache-treatment evidence for their mode, and successful removal of the unique ephemeral
fixture root before report validation returns. These correctness, memory, and cleanup checks apply
even when performance gates are disabled.

The save phase additionally fails closed on an incomplete durable result, compaction error, unchanged
generation, missing stale generation, maintenance that does not report busy while pinned, failed
prune during the reader gap, wrong replacement generation, any failed/stale/rejected load, incorrect
delta contents, nonzero final reservation, or a rotation count other than two. This follows the same
reader-end-mark principle documented for [SQLite WAL checkpoints](https://www.sqlite.org/wal.html):
long-lived readers constrain destructive checkpoint progress. Heartstead is not using SQLite here;
its immutable generation store chooses an explicit process-local lease and retryable busy result
instead of letting maintenance wait behind live streaming.

The default performance gates are:

| Metric | Default limit |
| --- | ---: |
| Generated near-ring interest-to-publication P95 | 250 ms |
| Teleport target-ring interest-to-publication P95 | 1,000 ms |
| In-memory saved-delta interest-to-publication P95 | 250 ms |
| File-backed saved-delta interest-to-publication P95 | 250 ms |
| File-backed payload-read P95 | 25 ms |
| File-backed generation-index open P95 | 100 ms |
| Maximum owner publication update | 500 us |
| Save-under-streaming interest-to-publication P95 | 250 ms |
| Owner-side save submission | 0.25 ms |
| Request-to-durable save acceptance | 5,000 ms |
| Complete background save compaction | 75,000 ms |

`--enforce-gates` evaluates these limits and returns exit code 3 after still writing the full report
when a limit is exceeded. The save gates are evaluated only when `--save-under-streaming` is present;
the expensive phase runs once rather than once per workload/repetition. Scheduler defaults remain
independently bounded at two workers, four
active/completed requests, 64 MiB measured reservation per request, 256 MiB aggregate reservation,
two publications per owner update, and 500 us per update.

## Schema v4 save-under-streaming calibration

Three independent clean-tracked-tree Release processes at revision
`9c63c3ba6fa0bb1ca457addaab6c453a111bed79` ran:

```text
cmake -S . -B build/default-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/default-release --target heartstead_chunk_streaming_benchmark -j2
build/default-release/apps/chunk_streaming_benchmark/heartstead_chunk_streaming_benchmark \
  --workload file_delta_warm \
  --physical-records 16384 \
  --physical-fixture-parent build/default-release/benchmarks/physical-fixtures \
  --warmup 0 \
  --repetitions 1 \
  --save-under-streaming \
  --enforce-gates \
  --output build/default-release/benchmarks/chunk-streaming-9c63c3b-save-under-load-16384-runN.json
```

Each process created a fresh 16,384-record generation, retained one ordinary 49-chunk warm physical
run, and then ran one separate 49-chunk contention phase while publishing the complete fixture
snapshot. There are no contention warmups or hidden phase repetitions. Raw reports remain outside
Git under `build/default-release/benchmarks/`.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `9c63c3ba6fa0bb1ca457addaab6c453a111bed79` |
| Source state | clean tracked tree in every retained run |
| Fixture payload | 16,384 records, 1,441,596 encoded bytes |

| Gated metric | Run 1 | Run 2 | Run 3 | Median process | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Concurrent interest-to-publication P95 | 35.153 ms | 47.122 ms | 38.225 ms | 38.225 ms | 250 ms |
| Concurrent physical payload-read P95 | 0.018100 ms | 0.030502 ms | 0.019900 ms | 0.019900 ms | 25 ms |
| Pinned generation-index open | 24.658 ms | 29.724 ms | 14.927 ms | 24.658 ms | 100 ms |
| Maximum contention owner publication | 40 us | 25 us | 49 us | 40 us | 500 us |
| Owner save submission | 0.027907 ms | 0.056619 ms | 0.018489 ms | 0.027907 ms | 0.25 ms |
| Request-to-durable acceptance | 15.017 ms | 22.278 ms | 16.730 ms | 16.730 ms | 5,000 ms |
| Worker durable operation | 14.951 ms | 22.160 ms | 16.647 ms | 16.647 ms | component only |
| Complete background compaction | 45,503.030 ms | 45,355.154 ms | 45,370.637 ms | 45,370.637 ms | 75,000 ms |

| Reported non-gated work | Run 1 | Run 2 | Run 3 | Median process |
| --- | ---: | ---: | ---: | ---: |
| Pre-interest fixture snapshot clone | 0.583 ms | 0.749 ms | 0.366 ms | 0.583 ms |
| Initial durable fixture setup | 46,222.650 ms | 45,458.681 ms | 45,512.850 ms | 45,512.850 ms |
| Save worker total | 45,518.003 ms | 45,377.349 ms | 45,387.307 ms | 45,387.307 ms |

Every process passed every regular and contention gate. Each contention phase published all 49
requested pinned-generation deltas with zero failed, stale, or rejected loads. Load reservations
reached the configured 256 MiB aggregate bound and returned to zero. Each full save was durably
accepted and compacted from `generation_1` to `generation_2`; destructive pruning returned busy while
the first reader was pinned, the explicit null-source gap allowed the stale generation to be removed,
the replacement reader verified every target payload, and exactly two source rotations were
recorded. Every unique fixture directory was removed before report validation.

The ordinary warm physical run P95 ranged from 36.813 to 42.283 ms. The concurrent range is similar,
but three local processes do not establish absence of I/O interference on other hardware. Most
importantly, the 0.366–0.749 ms fixture clone is not production capture evidence. The benchmark starts
with an already materialized typed snapshot; capturing a large, mutable live world on the owner
thread remains a separate M5 risk and must not be folded into the passing 0.25 ms submission claim.

## Historical schema v3 physical-file calibration

The retained 2026-08-01 calibration was produced from a clean tracked tree with:

```text
cmake -S . -B build/default-release -DCMAKE_BUILD_TYPE=Release
cmake --build build/default-release --target heartstead_chunk_streaming_benchmark -j2
build/default-release/apps/chunk_streaming_benchmark/heartstead_chunk_streaming_benchmark \
  --workload all \
  --physical-records 16384 \
  --physical-fixture-parent build/default-release/benchmarks/physical-fixtures \
  --enforce-gates \
  --output build/default-release/benchmarks/chunk-streaming-6b94643-physical-runN.json
```

Each process used two warmups and nine retained runs for each of the five Linux workloads. A radius
of four contains 49 target chunks, producing 2,205 raw samples per process. The physical fixture
contained 16,384 indexed deltas and 1,441,596 encoded payload bytes. The owner update cadence was
1 ms.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `6b94643bef69fb0737af87cbae5b1407f6309aac` |
| Source state | clean tracked tree in every retained run |

Raw reports remain outside Git under `build/default-release/benchmarks/` as
`chunk-streaming-6b94643-physical-run{1,2,3}.json`.

| Workload | Run 1 full P95 | Run 2 full P95 | Run 3 full P95 | Median process P95 | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Near generated ring | 37.694 ms | 39.801 ms | 36.573 ms | 37.694 ms | 250 ms |
| Teleport target ring | 40.755 ms | 41.022 ms | 37.716 ms | 40.755 ms | 1,000 ms |
| In-memory saved-delta ring | 38.596 ms | 38.636 ms | 36.623 ms | 38.596 ms | 250 ms |
| File delta, warm | 36.584 ms | 38.747 ms | 37.544 ms | 37.544 ms | 250 ms |
| File delta, cache-drop advised | 41.862 ms | 42.921 ms | 44.157 ms | 42.921 ms | 250 ms |

| Physical stage | Run 1 P95 | Run 2 P95 | Run 3 P95 | Median process P95 | Gate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Warm payload read | 0.036044 ms | 0.042166 ms | 0.044043 ms | 0.042166 ms | 25 ms |
| Warm index open | 13.466661 ms | 12.808863 ms | 12.175042 ms | 12.808863 ms | 100 ms |
| Advice-accepted payload read | 0.201308 ms | 0.238455 ms | 0.250318 ms | 0.238455 ms | 25 ms |
| Advice-accepted index open | 13.890961 ms | 16.950059 ms | 15.957277 ms | 15.957277 ms | 100 ms |

Every process passed every gate, published all 49 requested chunks per retained run, reported zero
failed/stale/rejected loads, and returned its measured reservation to zero. Each process recorded
441 warm preloads and 459 accepted cache-advice calls out of 459 attempts. Fixture cleanup
succeeded. The worst owner update across the three processes was 126 us.

Fixture setup took 48.384, 48.179, and 48.324 seconds. That time is deliberately retained rather
than hidden: the current production generation writer durably emits every per-chunk file and makes
the large-fixture setup roughly linear in record count. It was the strongest signal in this slice
and motivated the later append-oriented streamed write path and schema-v4 save-under-load gates.

### Record-count sweep

Two additional clean-revision probes held the workload shape constant and varied the physical
generation size:

| Indexed records | Encoded bytes | Fixture setup | Warm open P95 | Warm read P95 | Advice-accepted open P95 | Advice-accepted read P95 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 128 | 11,068 | 0.405 s | 0.081690 ms | 0.035943 ms | 0.402097 ms | 0.126992 ms |
| 4,096 | 360,252 | 12.020 s | 1.837520 ms | 0.039639 ms | 2.640288 ms | 0.122008 ms |
| 16,384 | 1,441,596 | 48.324 s | 12.808863 ms | 0.042166 ms | 15.957277 ms | 0.238455 ms |

The production reader's per-target payload P95 is essentially flat in this sweep. The one-time
sorted-index parse grows with table size but remains far below its gate. The production writer's
setup time, not the read path, is the observed persistence-scale bottleneck.

## Historical schema v2 baseline

Before physical-file workloads were added, three clean Release processes at revision
`9ddfd9cef43f3dd2b01d241619586275051f1f40` measured median process-level P95 of 35.398 ms for the
near ring, 37.645 ms after teleport, and 37.694 ms for the in-memory indexed saved-delta ring. The
worst owner publication update was 55 us. These reports remain outside Git as
`chunk-streaming-9ddfd9c-run{1,2,3}.json`; they are retained for historical comparison, not mixed
with schema v3 results.

CPU frequency, desktop workload, power policy, and thermal state were not controlled in either
calibration. These values are local absolute-gate evidence with clear headroom, not a portable
hardware guarantee. Admission deferrals are expected and visible because the required ring is
intentionally larger than scheduler capacity.

## Boundary and remaining M5 work

The calibration proves generated block data and one-edit deltas from the production indexed file
reader can reach authoritative resident publication through the bounded asynchronous path on the
declared Linux filesystem. It covers a deliberately warmed state and a state in which Linux
accepted cache-drop advice. It does not prove a guaranteed cold page cache, physical-device cache
miss, another filesystem or storage medium, network storage, or behavior under concurrent save
writes.

This benchmark also stops before lighting, collision cooking, replication transport, client
residency, meshing, GPU upload, draw eligibility, and display. Calling it alone "time-to-visible"
would therefore be incorrect. The companion
[chunk render-readiness benchmark](chunk_render_readiness_benchmarks.md) carries a fixed generated
required population through this scheduler, asynchronous meshing, RHI upload, exact GPU-cache
residency, visibility filtering, and production draw-command construction. Resident collision
publication and whole-field relight convergence have separate headless/Jolt gates in
[Voxel response benchmarks](voxel_response_benchmarks.md).

The companion [chunk delta journal benchmark](chunk_delta_journal_benchmarks.md) verifies that
streamed foreground writes append one durable entry instead of rewriting the full table, while also
showing that complete checkpoint remains proportional to table size. Schema v4 now adds a real
save-under-streaming generation publication, pinned-view maintenance gate, explicit reader gap,
prune, and source rotation. M5 still requires production-scale live snapshot capture,
general generated-world runtime-controller adoption, guaranteed-cold and
multi-filesystem/media validation where environments permit it, and GPU
execution/presentation/display timing beyond the draw-eligibility endpoint. The live
renderer-proof controller is currently the application path that exercises the asynchronous
loader.
