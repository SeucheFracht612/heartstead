# Renderer benchmarks

This page defines the maintained renderer measurement workflow and preserves explicitly dated
historical baselines. It replaces milestone-specific reports whose useful conclusions now belong in
one reproducible guide.

## Benchmark application

Build an optimized runner:

```bash
cmake --preset default-release
cmake --build --preset default-release --target heartstead_render_benchmark
```

Build the optimized, symbolized Tracy client instrumentation when collecting a hierarchical trace:

```bash
cmake --preset profiling-release
cmake --build --preset profiling-release --target heartstead_render_benchmark
```

The profiling preset enables the optional vcpkg `profiling` feature and links Tracy in on-demand
mode. Start a compatible Tracy profiler and then launch the binary when a live capture is needed.
Ordinary presets compile the permanent profiling call sites to no-ops.

Run headlessly:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene mountains --warmup 120 --frames 1000 --radius 2 \
  --output build/benchmarks/mountains.json
```

Run the native Vulkan backend and export CSV:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene checkerboard --warmup 120 --frames 1000 --radius 1 \
  --format csv --output build/benchmarks/checkerboard-vulkan.csv
```

Measure host-observed presentation completion only when diagnosing that endpoint:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --presentation-timing --no-validation --scene mountains \
  --warmup 120 --frames 600 --radius 8 \
  --output build/benchmarks/mountains-present-completion.json
```

Use `--list-scenes` and `--help` rather than relying on an old copied option list. Add
`--reference-mesher` to compare the readable correctness mesher against the optimized greedy path.
The runner is uncapped unless `--frame-cap` is explicitly supplied.

Enforce a starting hardware tier in automation:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene mountains --warmup 120 --frames 1000 --radius 2 \
  --budget minimum --output build/benchmarks/mountains-minimum.json
```

`--budget compatibility|minimum|mainstream|high-end` evaluates median, P95, P99, maximum frame,
maximum upload bytes, and mean GPU time when GPU timestamps exist. On `rapid-edits`, it additionally
requires at least one completed edit sample, no final pending or abandoned interval, edit-to-visible
P95 at most 50 ms, and at most 1.1 built meshes per publication. Results are written before a failing
gate exits with status 2. `none` is the default so exploratory runs do not silently become release
gates. `mass-excavation` remains an overload diagnostic and does not claim the sustainable-edit SLO.

## Workloads

The deterministic catalog includes static and stress coverage for:

- flat terrain;
- mountains;
- caves;
- checkerboard/worst-case face separation;
- cross-plane forest/foliage;
- terrain material bands, nine surface states, and authored slopes;
- a 128-light settlement grid with mixed point/spot lights and shadow selection;
- the integrated starting biome with rolling terrain, river and large water, instanced meadow,
  forest and crop vegetation, rain, smoke, embers, atmosphere, and a shadowed fire light;
- active voxel-fluid simulation and presentation;
- a 50,000-particle stress scene;
- equipped animated characters plus stateful workshop machines;
- one deterministic single-block edit every two frames, including periodic chunk-boundary edits;
- a separate 32-edits-per-frame mass-excavation overload for coalescing and stale-work pressure;
- high-speed flythrough/streaming;
- load/unload churn;
- large world coordinates;
- resize/minimize behavior.

Integer/hash generation and frame-indexed stress schedules make runs reproducible from scene, seed,
radius, and frame configuration. Before measurement, the runner settles the initially loaded chunks
to resident meshes so streaming/edit tests measure replacement and churn rather than accidental
initial starvation. Initial voxel lighting is also settled once. During warm-up and measurement the
lighting system advances exactly one bounded update per simulation frame; the benchmark never waits
for an entire relight field inside a frame. At the warm-up boundary, chunk performance counters and
the latency distribution reset without cancelling or discarding work already in flight.

## Timing semantics

Warm-up frames are excluded. Output records per-frame renderer counters plus summary statistics,
including median, p95, p99, maximum frame time, 1% and 0.1% low FPS, and CPU/GPU subsystem timing.
CPU zones distinguish synchronization, extraction, culling, draw-list and command construction,
snapshot capture, meshing, upload preparation/copy, backend recording, and blocking waits.

Vulkan timestamps are asynchronous. Every GPU result carries the source frame and result latency;
do not align a delayed GPU sample with the CPU frame that happened to receive it. The headless
backend reports GPU timing unavailable while preserving the same CPU/counter schema.

`--presentation-timing` is a Vulkan-only, opt-in diagnostic. The device must expose and enable both
`VK_KHR_present_id` and `VK_KHR_present_wait`. The renderer assigns a non-zero, strictly increasing
ID to each queued present and waits up to one second for that ID with `vkWaitForPresentKHR`; a
missing capability, missing entry point, timeout, or unexpected wait result fails closed. Each raw
frame retains validity, ID, and elapsed wait, while the summary computes mean, median, P95, P99, and
maximum over valid samples only. The interval begins immediately before `vkQueuePresentKHR` and
ends when the completion wait returns, so it includes the queue call and the host wait.

This mode deliberately serializes every presentation and therefore changes frame pacing and
`cpu_frame_ms`. It is not an ordinary throughput mode, an input-to-photon measurement, a precise
presentation timestamp, or proof of physical scan-out. The normal path does not enable these
extensions, wait for presentation, or take a presentation clock sample. Vulkan only queues a
presentation through [`vkQueuePresentKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueuePresentKHR.html);
the completion operation and its guarantees come from
[`VK_KHR_present_wait`](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_present_wait.html)
and [`vkWaitForPresentKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkWaitForPresentKHR.html),
with IDs supplied by
[`VkPresentIdKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/VkPresentIdKHR.html).

JSON and CSV schema v4 carry scene/run configuration plus engine version, Git commit and tracked
dirty state, build configuration, compiler, platform, architecture, OS, CPU model/logical CPUs,
Tracy state, GPU, driver information, and numeric Vulkan API/driver versions. Budget evaluation and
violations are retained with the raw frames. Additive schema-v4 fields retain whether presentation
timing was requested and supported plus its per-frame and aggregate samples. Schema v4 records
bounded chunk-mesh
invalidation-to-resident latency: each event begins at the monotonic dirty-region mark and ends only
when the exact requested mesh-stage revision is uploaded and published. It includes boundary
neighbors, reports a fixed 256-completion rolling median/P95/P99, and separates pending, coalesced,
completed, and abandoned intervals. After measured frames stop generating edits, a bounded 240-frame
drain lets outstanding intervals publish; drain frames do not enter frame-time statistics, and any
interval still pending is retained as censored work and fails the `rapid-edits` gate. Final completed,
pending, sample, drain, mesh-job/build/publication, and amplification counters therefore cannot turn
an empty latency distribution into a pass. "Visible" here means resident and eligible for the same
frame's draw list, not display scan-out. Keep the raw output with any conclusion.

The first permanent trace surface includes runtime/client/server frames, renderer synchronization,
extraction, visibility, draw construction, command construction/submission, chunk snapshots,
meshing, upload preparation/copy, worker jobs with name and ID, voxel lighting, collision cooking,
and chunk streaming. Tracy plots expose general job backlog, chunk mesh/upload queue depth, pending
edit intervals, and rolling edit-to-visible P95.
Extend this same hierarchy when adding pipeline stages rather than creating one-off timer systems.

## Comparison procedure

1. Use the same commit or state the exact before/after commits.
2. Use `default-release` and record compiler/toolchain.
3. Hold machine, power mode, thermal state, driver, resolution, backend, scene, seed, radius, frame
   counts, and frame cap constant.
4. Run more than once and report run-to-run spread, not only the best result.
5. Inspect subsystem counters before attributing a frame-time change.
6. Separate CPU, GPU, meshing, upload, streaming, and synchronization conclusions.
7. Use the reference mesher only as an otherwise-identical geometry baseline.
8. Treat Vulkan validation state as part of the configuration.
9. Never compare a serialized `--presentation-timing` run with an ordinary throughput run.
10. Preserve output files; do not transcribe only a headline FPS number.
11. Add a new dated baseline below only when it remains useful for future decisions.

## Vulkan presentation-completion calibration — 2026-08-02

This calibration verifies the new completion endpoint and establishes its run-to-run behavior. It
does not redefine the normal renderer frame budgets because each sample synchronously waits for its
own presentation to complete.

- **Build:** clean `3b02b3fa157a447383db92015a3832d658fe3279`, GCC 13.3.0, Release
- **Machine:** Intel Core Ultra 7 258V, 8 logical CPUs, Intel Graphics (LNL), Mesa 25.2.8,
  Linux 6.17.0-1030-oem
- **Configuration:** Vulkan, validation disabled, 1920x1080, radius 8, 120 warm-up frames,
  600 measured frames, immediate requested/uncapped, three independent processes per scene
- **Command:** `heartstead_render_benchmark --vulkan --presentation-timing --no-validation
  --scene SCENE --width 1920 --height 1080 --radius 8 --warmup 120 --frames 600 --budget none`
- **Raw output:** `build/default-release/benchmarks/m7-{flat,mountains}-present-completion-run{1,2,3}.json`

| Scene/run | Frame median ms | Frame P95 ms | Mean GPU ms | Present mean ms | Present median ms | Present P95 ms | Present P99 ms | Present max ms | Valid/total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Flat 1 | 8.266 | 10.266 | 3.507 | 4.438 | 3.757 | 6.823 | 7.608 | 8.485 | 600/600 |
| Flat 2 | 7.800 | 10.009 | 3.325 | 4.085 | 3.357 | 6.304 | 7.149 | 8.702 | 600/600 |
| Flat 3 | 7.963 | 10.064 | 3.471 | 4.195 | 3.396 | 6.687 | 7.227 | 7.993 | 600/600 |
| Mountains 1 | 14.119 | 15.946 | 8.534 | 9.195 | 8.984 | 11.735 | 13.472 | 14.366 | 600/600 |
| Mountains 2 | 14.158 | 15.813 | 8.485 | 9.119 | 9.022 | 11.403 | 13.555 | 14.235 | 600/600 |
| Mountains 3 | 13.839 | 15.789 | 8.551 | 9.308 | 9.111 | 11.844 | 12.928 | 13.986 | 600/600 |

All 3,600 measured waits were valid. Each process retained the contiguous measured ID range
195-794: IDs 1-74 covered initial settlement and 75-194 covered warm-up. Flat process medians varied
by 0.400 ms and P95 by 0.519 ms; mountains medians varied by 0.128 ms and P95 by 0.441 ms. A separate
validation-enabled Debug smoke at 640x360 completed 30/30 waits without a Vulkan validation message.

The result closes a generic host-observed queue-call-to-presentation-completion endpoint. It does
not yet correlate a voxel edit or required-chunk draw with the presentation ID that first contains
it, and it cannot observe compositor-to-panel scan-out. Those require a dedicated response workload
and, for physical display latency, external or platform-specific measurement.

## Near-terrain multi-draw-indirect rejection — 2026-08-02

This trace-gated experiment tested whether replacing compatible near-terrain and terrain-shadow
draws with Vulkan multi-draw-indirect (MDI) calls improved a shipping configuration. The prototype
used a four-slot persistently mapped staging ring, copied commands and 16-byte per-draw origins into
device-local buffers in the consuming graphics submission, and indexed draw data through
`firstInstance`. It had bounded 16,384-draw capacity, direct fallback, and zero upload waits,
overflows, or failures in the measured runs. The experiment was removed because the complete frame
regressed despite reducing command-recording time.

- **Build:** dirty experimental worktree based on `29cc3774bbe2760c28505a53e6c404ab49149df4`,
  GCC 13.3.0, Release; the rejected implementation is not in the repository
- **Machine:** Intel Core Ultra 7 258V, 8 logical CPUs, Intel Graphics (LNL), Mesa 25.2.8,
  Linux 6.17.0-1030-oem
- **Configuration:** Vulkan, validation disabled, 1920x1080, radius 8, 120 warm-up frames,
  600 measured frames, immediate/uncapped
- **Commands:** `heartstead_render_benchmark --vulkan --no-validation --scene SCENE --width
  1920 --height 1080 --radius 8 --warmup 120 --frames 600 --budget none`, with the prototype's
  `--no-near-terrain-batching` switch for direct controls
- **Raw output:** `build/default-release/benchmarks/m7-{mountains,flat}-{direct,near-mdi}-*.json`

| Scene/path | Median ms | P95 ms | Mean CPU ms | Mean GPU ms | Record ms | GPU opaque ms | Batched source draws/calls |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Mountains direct 1 | 18.209 | 22.944 | 18.469 | 14.227 | 0.678 | 6.830 | 0/0 |
| Mountains direct 2 | 18.326 | 23.596 | 18.732 | 14.602 | 0.694 | 7.230 | 0/0 |
| Mountains near MDI | 21.968 | 32.305 | 23.081 | 20.897 | 0.373 | 11.469 | 902/12 |
| Flat direct | 5.043 | 7.851 | 5.298 | 3.409 | 0.514 | 1.093 | 0/0 |
| Flat near MDI | 5.843 | 15.762 | 7.333 | 5.580 | 0.275 | 0.912 | 977/4 |

Against the mean of the two adjacent mountains controls, MDI reduced recording by 45.5% but raised
mean CPU time by 24.1%, mean GPU time by 45.0%, median by 20.3%, and P95 by 38.8%. On flat terrain,
recording fell 46.4%, while CPU rose 38.4%, GPU rose 63.7%, median rose 15.9%, and P95 rose 100.8%.
The production-mode direct path recorded roughly one thousand logical terrain/shadow draws in only
0.51-0.69 ms. Earlier validation-enabled traces attributed roughly 4-6 ms to the same area, so
validation overhead was not accepted as a shipping bottleneck.

The durable decision is to retain direct near-terrain draws and the already effective far-terrain
indirect path. The renderer now independently checks `multiDrawIndirect`,
`drawIndirectFirstInstance`, and the device's `maxDrawIndirectCount`; far-terrain groups are split
at that limit. This follows the separate Vulkan feature and limit contracts described by the
[Vulkan physical-device feature reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceFeatures.html),
[indexed-indirect command reference](https://docs.vulkan.org/refpages/latest/refpages/source/VkDrawIndexedIndirectCommand.html),
and [Khronos MDI sample](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html).
Meshlets, compute meshing, descriptor indexing, and broader GPU-driven visibility remain deferred
until a validation-off capture identifies a production bottleneck they can address.

## Voxel rapid-edit baseline — 2026-08-01

This baseline exercises the sustainable fixed-arrival edit workload and the schema-v4
right-censoring safeguards. It toggles one guaranteed state-changing voxel every two frames, adds
periodic chunk-boundary invalidations, advances one bounded lighting update per frame, and enforces
the `minimum` profile plus the 50 ms mesh-publication P95 and 1.1 build-amplification limits.

- **Build:** clean `a89fd65358ea2a61c36984c7559f9c17e8397808`, GCC 13.3.0, Release
- **Machine:** Intel Core Ultra 7 258V, 8 logical CPUs, Linux 6.17.0-1030-oem
- **Configuration:** headless, greedy mesher, 1280×720, radius 2, 120 warm-up frames, 1,000
  measured frames, 60 FPS cap, `minimum` budget
- **Command:** `heartstead_render_benchmark --scene rapid-edits --warmup 120 --frames 1000
  --radius 2 --frame-cap 60 --budget minimum`
- **Raw output:** `build/default-release/benchmarks/rapid-edits-a89fd65-run{1,2,3}.json`

| Run | Frame median ms | Frame P95 ms | Frame P99 ms | Edit median ms | Edit P95 ms | Edit P99 ms | Edit max ms | Jobs/built/published | Amplification | Final pending/abandoned | Gate |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 1 | 1.310 | 2.274 | 2.580 | 17.019 | 19.491 | 20.081 | 20.229 | 600/600/600 | 1.000 | 0/0 | pass |
| 2 | 1.324 | 2.360 | 2.732 | 17.022 | 19.636 | 20.320 | 20.431 | 600/600/600 | 1.000 | 0/0 | pass |
| 3 | 1.340 | 2.290 | 2.654 | 17.040 | 19.854 | 20.086 | 20.363 | 600/600/600 | 1.000 | 0/0 | pass |

Each run retained the full 256-sample rolling window, completed 600 measured edit-to-visible
intervals, needed no tail-drain frame, and passed every evaluated CPU/upload/edit gate. GPU timing is
intentionally unavailable on the deterministic headless backend. The edit-P95 spread was 0.362 ms;
the result clears the 50 ms target without evidence that slab or microbrick rebuild complexity is
needed for this workload.

## Renderer V2 terrain-material baseline — 2026-07-30

This closure run measures the unified terrain PBR path, three aligned mipmapped texture arrays,
world-stable macro/variant mapping, nine shared surface-state layers, voxel AO, shadows, and mixed
greedy/authored-slope geometry.

- **Build:** dirty Task 3 worktree, GCC 13.3.0, Release
- **Machine:** Intel Lunar Lake integrated graphics, Mesa 25.2.8, Linux 6.17.0-1030-oem
- **Configuration:** Vulkan, validation requested, 1280×720, radius 1, 120 warm-up frames,
  300 measured frames, immediate/uncapped, three sequential runs
- **Command:** `heartstead_render_benchmark --vulkan --scene terrain-materials --warmup 120
  --frames 300 --radius 1`
- **Raw output:** `build/default-release/benchmarks/terrain-materials-task3-run{1,2,3}.json`

| Run | Median ms | p95 ms | p99 ms | 1% low FPS | 0.1% low FPS | Max ms | Mean GPU ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2.653 | 5.011 | 6.106 | 151.0 | 142.8 | 7.001 | 2.784 |
| 2 | 2.779 | 6.532 | 7.432 | 122.8 | 119.4 | 8.375 | 3.269 |
| 3 | 2.876 | 6.430 | 7.922 | 114.9 | 99.9 | 10.006 | 3.345 |

Each measured frame held nine resident chunks, 3,251 terrain triangles, seven runtime terrain
materials, 146 total draws, and no pending or failed chunk work. The spread is retained rather than
selecting the fastest run; use the raw outputs for subsystem attribution.

## Renderer V2 lighting baseline — 2026-07-30

This closure run measures the new tiled direct-lighting, four-cascade directional-shadow, two-map
local spotlight-shadow, SSAO, FXAA, bloom, and tone-map frame on the deterministic 128-light scene.

- **Build:** dirty Task 2 worktree, Clang 18.1.3, Release
- **Machine:** Intel Lunar Lake integrated graphics, Mesa 25.2.8, Linux 6.17.0-1030-oem
- **Configuration:** Vulkan, validation requested, 1280×720, radius 1, 120 warm-up frames,
  300 measured frames, immediate/uncapped
- **Command:** `heartstead_render_benchmark --vulkan --scene light-heavy --warmup 120 --frames
  300 --radius 1`
- **Raw output:** `build/task2-release/benchmarks/light-heavy-task2.json`

| Median ms | p95 ms | p99 ms | 1% low FPS | 0.1% low FPS | Max ms | Mean GPU ms |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1.650 | 3.522 | 5.360 | 169.0 | 161.4 | 6.196 | 1.536 |

The measured frame held 128 submitted local lights, nine resident terrain chunks, and both selected
local shadow slots. Treat this as the first V2 lighting reference, not a cross-machine comparison
with the V1 data below.

## Historical renderer V1 baseline

The following data is retained from the renderer V1 closure run. It is historical evidence, not a
current performance guarantee.

- **Date:** 2026-07-17
- **Build:** GCC 13.3, Release
- **Machine:** AMD Ryzen 7 5800U (8 cores/16 threads), Radeon RADV Renoir, Mesa 25.2.8,
  13 GiB RAM, Linux Mint 22.3, kernel 6.17.0-40
- **Native configuration:** Vulkan, 1920×1080, immediate/uncapped

| Scene/configuration | Median ms | p95 ms | p99 ms | 1% low FPS | 0.1% low FPS | Max ms | Mean GPU ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Mountains radius 4, run 1 | 1.762 | 2.801 | 2.852 | 335.8 | 320.7 | 3.118 | 1.442 |
| Mountains radius 4, run 2 | 1.775 | 2.809 | 2.895 | 342.0 | 336.5 | 2.972 | 1.440 |
| Churn radius 4 | 0.840 | 1.744 | 1.908 | 498.5 | 459.7 | 2.175 | 0.538 |
| Rapid edits radius 4 | 3.791 | 4.377 | 4.785 | 172.1 | 157.5 | 6.349 | 0.859 |
| Flat radius 8 | 0.894 | 1.872 | 2.019 | 448.0 | 419.4 | 2.384 | 0.593 |

The preserved greedy/reference comparison from the same optimization era showed:

| Metric | Reference mesher | Greedy mesher |
| --- | ---: | ---: |
| Resident triangles in deterministic flat nine-chunk scene | 43,776 | 108 |
| Mesh bytes | 2,363,904 | 5,832 |
| Release worker meshing | 2.571 ms | 1.890 ms |
| Release frame median | 3.112 ms | 3.053 ms |
| Release frame p95 | 3.494 ms | 3.262 ms |

The durable conclusion is structural: greedy merging dramatically reduced flat full-cube geometry,
while total frame time was already dominated by more than raw triangle count. Future optimization
should use current subsystem measurements instead of assuming the same bottleneck.

## Integrated environment workload

Use `starting-biome` to measure the environmental systems together:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene starting-biome --warmup 120 --frames 300 --radius 1 \
  --output build/benchmarks/starting-biome.json
```

The scene loads production cooked content, evaluates environment profiles, and updates vegetation,
large water, weather particles, smoke, and embers through the same interfaces as gameplay. Its
fixed seed supplies thousands of grass, flowers, crops, trees, bushes, and magical plants without
one draw per plant. Record visible/culled vegetation instances, instance draws, active particles,
particle drops, local lights, GPU pass timings, and validation state when publishing a result.

The small Debug/Vulkan closure smoke test on 2026-07-31 used 640×360, radius 1, one warm-up and two
measured frames. It completed with no Vulkan validation messages, approximately 2,700 visible
vegetation instances in seven instanced draws, and a mean GPU frame time of roughly 7.6 ms. This
confirms integration and validation only; it is not an optimized performance baseline.

## Large-world runs

The `flythrough`, `churn`, and `large-coordinates` scenes are the authoritative large-world stress
runs. Their JSON/CSV samples include visibility hierarchy traversal, far-terrain residency and
uploads, indirect draw counts, GPU arena pressure, and driver-reported memory usage/budget. See
`docs/architecture/large_world_rendering.md` for the systems exercised by each counter.

## Coverage and visual regression

`renderer/benchmark/benchmark_coverage.*` maps every required workload to a deterministic scene.
The runner supports `--capture` and `--compare` for Vulkan display-image regression; see
[Visual regression](../dev/visual_regression.md).

The 320x180 debug Vulkan capture smoke on 2026-08-01 measured 2.536 ms GPU for the 2,367-object
starting-biome snapshot and 2.922 ms GPU for the 3,072-object character-workshop snapshot. These are
single startup samples validating instrumentation and capture, not release performance claims.

## Publishing a new baseline

Append a dated section with commit SHA, dirty-tree state, compiler, build preset, CPU/GPU, RAM, OS,
kernel, driver, display/resolution, backend, validation state, command lines, repeat count, and links
to raw output artifacts. Explain the decision the data supports and any source of noise. Never
replace an old machine's values with new values under the same label.
