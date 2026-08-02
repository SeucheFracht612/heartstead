# Chunk render-readiness benchmarks

Status: required generated chunks pass the interest-to-exact-draw-command, upload-preparation,
synchronous-wait, and mesh-amplification gates on the declared reference CPU with both the
deterministic headless renderer and the selected Vulkan physical device. This is a draw-eligibility
gate, not a GPU-execution, presentation, or display-scan-out measurement.

The `heartstead_chunk_render_readiness_benchmark` executable drives the production
`ChunkLoadScheduler`, `ChunkDatabase`, `ChunkRenderSystem`, asynchronous mesh workers,
`ChunkGpuCache`, render-device uploads, visibility hierarchy, and draw-list builder. Report schema
version 1 retains every required chunk's lifecycle sample plus source, build, machine, backend,
device, driver, workload, queue, memory, and teardown evidence.

## Timing contract

The default workload declares all 13 coordinates in a circular radius-two required zone at one
`steady_clock` instant. Only four load requests can be active at once, but deferred requests retain
that original interest timestamp. The clock therefore includes bounded admission delay rather than
starting only when capacity becomes available. This follows the same principle as
[wrk2's coordinated-omission treatment](https://github.com/giltene/wrk2): latency begins when work
was supposed to enter the system, so saturation cannot erase delayed samples.

Each workload uses a deterministic, non-empty shallow-terrace corpus. The first owner update runs
immediately and subsequent updates follow a fixed 16,667 us cadence. The endpoint for each chunk is
the first production draw list containing a draw command for that exact coordinate after all of the
following are simultaneously true:

- the chunk identity and load generation still match the required publication;
- the mesh stage's resident request is current;
- content, render-table, and neighborhood dependency revisions match the GPU-cache entry;
- the resident GPU entry has non-empty drawable vertex and index buffers;
- the current draw list contains that chunk's collision-checked deterministic terrain seed.

The resulting `interest_to_draw_eligibility_us` spans admission, worker queueing, generation,
private preparation, owner publication, snapshot construction, asynchronous meshing, stale-result
rejection, RHI upload, cache publication, distance/frustum/hierarchy filtering, and draw-list
construction. Publication-to-mesh and mesh-to-draw intervals remain separately attributable.

Wall-clock timing is intentional because the pipeline uses internal worker threads; the
[Google Benchmark guide](https://google.github.io/benchmark/user_guide.html) makes the same
distinction and also documents warmups and retained repetitions. Heartstead implements those
measurement choices directly without depending on that library. Two complete workloads warm the
harness and are discarded; nine later workloads and all 117 chunk samples are retained.

The endpoint does not prove that a GPU executed the copy or draw, that a swapchain image was
presented, or that a display scanned it out. Vulkan queues execute submitted work asynchronously,
as specified in [Vulkan fundamentals](https://docs.vulkan.org/spec/latest/chapters/fundamentals.html),
and command buffers remain pending until device execution completes, as described by the
[command-buffer lifecycle](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html). The JSON
therefore names the endpoint `first_exact_current_chunk_draw_command` and explicitly sets
`excludes_gpu_execution_and_scanout` to true.

## Backends, gates, and invariants

`--backend headless|vulkan` is an explicit selection. An unavailable requested backend fails; the
harness never substitutes headless execution for Vulkan.

- Headless runs exercise production RHI buffer allocation, writes, ownership, GPU-cache
  publication, visibility, and draw-command construction in deterministic host memory. They do not
  measure a physical GPU or driver.
- Vulkan runs create physical buffers, stage and submit copies, and exercise the selected device and
  driver. Upload-call time still ends at host API return, not device completion. Reusing an upload
  context polls its fence first; only a genuinely unfinished context enters and records a blocking
  fence wait.

The default performance gates are:

| Metric | Default limit |
| --- | ---: |
| Required interest to exact current draw command P95 | 250 ms |
| Maximum owner-side upload preparation per update | 0.5 ms |
| Maximum synchronous GPU fence wait | 0 ms |
| Maximum mesh builds per published mesh | 2.5 |

`--enforce-gates` evaluates all four limits and returns exit code 3 after writing the complete JSON
report when a limit is exceeded. Upload preparation covers owner-side batch construction before the
RHI call. `maximum_upload_ms` and the complete owner/synchronize timings are also retained as
diagnostic evidence, but they are not mislabeled as GPU completion time.

Every run also fails closed, even without performance gates, unless every target produces exactly
one uncensored sample and the final world contains only current, exact, drawable meshes represented
in the current draw list. Load cancellations, stale/failed/rejected loads, mesh or upload failures,
abandoned invalidations, pending load/mesh/upload/edit-response work, leaked render resources,
nonzero final reservations, or snapshot/upload/residency budget violations invalidate the report.
Stale mesh results are permitted only as recorded superseded work; the amplification gate bounds
them and final exactness remains mandatory.

## Reproduction and provenance

Use an optimized build. A canonical headless process is:

```text
cmake --preset default-release
cmake --build --preset default-release \
  --target heartstead_chunk_render_readiness_benchmark -j2
build/default-release/apps/chunk_render_readiness_benchmark/heartstead_chunk_render_readiness_benchmark \
  --backend headless \
  --enforce-gates \
  --output build/default-release/benchmarks/heartstead-chunk-render-readiness-289ecfa-headless-runN.json
```

Replace `headless` with `vulkan` and the output label likewise for a physical-device run. The
retained calibration used the default seed, radius two, two warmups, nine retained repetitions,
13 chunks per repetition, 16,667 us cadence, and 5 s fail-closed workload timeout.

| Property | Value |
| --- | --- |
| Machine | Intel Core Ultra 7 258V, 8 logical CPUs |
| OS | Linux 6.17.0-1030-oem, x86-64 |
| Compiler/build | GCC 13.3.0, Release |
| Source revision | `289ecfa62429c92b0396bd34e777a10bc95aa2c3` |
| Source state | clean tracked tree in every retained run |
| Vulkan device | Intel(R) Graphics (LNL), vendor `0x8086`, device `0x64a0` |
| Vulkan driver | Intel open-source Mesa driver, Mesa 25.2.8-0ubuntu0.24.04.2 |

Raw reports remain outside Git under `build/default-release/benchmarks/` as
`heartstead-chunk-render-readiness-289ecfa-headless-run{1,2,3}.json`,
`heartstead-chunk-render-readiness-289ecfa-vulkan-run{1,2}.json`, and
`heartstead-chunk-render-readiness-289ecfa-vulkan-run3-retry.json`.

## 2026-08-01 calibration

| Backend/run | Draw P95 | Draw P99 | Upload preparation max | Upload call max | GPU wait max | Owner update max |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Headless 1 | 166.973 ms | 167.063 ms | 0.038948 ms | 0.047566 ms | 0 ms | 2.603 ms |
| Headless 2 | 166.857 ms | 166.968 ms | 0.039751 ms | 0.042830 ms | 0 ms | 2.356 ms |
| Headless 3 | 166.856 ms | 166.897 ms | 0.041536 ms | 0.040382 ms | 0 ms | 1.914 ms |
| Vulkan 1 | 166.956 ms | 167.460 ms | 0.039498 ms | 4.250456 ms | 0 ms | 5.856 ms |
| Vulkan 2 | 166.942 ms | 167.051 ms | 0.044885 ms | 5.528558 ms | 0 ms | 7.204 ms |
| Vulkan 3 retry | 166.955 ms | 167.249 ms | 0.044556 ms | 5.331448 ms | 0 ms | 7.148 ms |

All six retained processes pass every gate. Each has a maximum mesh-build amplification of 2.286.
Incremental face-neighbor arrivals conservatively supersede 18 already-built meshes per workload,
so every nine-run process records 162 stale mesh results. That is expected version rejection rather
than unresolved work: every final mesh is exact and current, and every queue, reservation, and
render-resource count returns to baseline.

The physical backend's 4.25–5.53 ms maximum upload call is the intentionally retained cold cost of
creating and mapping the workload's persistent 32 MiB staging buffer plus submitting the first
copy. It is visible in owner/synchronize time and is not hidden by a prewarm. The separately gated
upload-preparation cost remains below 0.045 ms, and no retained update blocks on a GPU fence.

One attempted third full Vulkan process hit the 5 s fail-closed timeout before it could produce a
valid report. The unchanged immediate retry passed, as did 20 additional independent one-workload
processes and one 100-workload lifecycle process containing 1,300 samples. No automatic retry was
added, and the failed attempt is not counted as a sample. The anomaly was not reproducible, but it
remains relevant if future hardware runs show a similar lifecycle stall.

CPU frequency, desktop workload, power policy, and thermal state were not controlled. These values
are a local absolute-gate calibration, not a portable hardware guarantee.

## Remaining M5 boundary

This workload closes the required generated-chunk path through exact CPU-side draw eligibility and
explicitly gates upload preparation and synchronous fence waits. It does not cover saved deltas,
large snapshot capture, general generated-world controller adoption, replication transport, GPU
execution timestamps, presentation timing, or display scan-out. The companion
[chunk streaming benchmark](chunk_streaming_benchmarks.md) covers warm and Linux cache-drop-advice
physical-file reads plus opt-in save-under-streaming generation rollover, but not guaranteed-cold,
multi-filesystem, or live-world snapshot-capture behavior. Those
remaining boundaries stay staged in the
[voxel optimization roadmap](voxel_optimization_roadmap.md).
