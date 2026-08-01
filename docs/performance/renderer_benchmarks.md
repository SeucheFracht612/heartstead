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
maximum upload bytes, and mean GPU time when GPU timestamps exist. Results are written before a
failing gate exits with status 2. `none` is the default so exploratory runs do not silently become
release gates.

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
for an entire relight field inside a frame.

## Timing semantics

Warm-up frames are excluded. Output records per-frame renderer counters plus summary statistics,
including median, p95, p99, maximum frame time, 1% and 0.1% low FPS, and CPU/GPU subsystem timing.
CPU zones distinguish synchronization, extraction, culling, draw-list and command construction,
snapshot capture, meshing, upload preparation/copy, backend recording, and blocking waits.

Vulkan timestamps are asynchronous. Every GPU result carries the source frame and result latency;
do not align a delayed GPU sample with the CPU frame that happened to receive it. The headless
backend reports GPU timing unavailable while preserving the same CPU/counter schema.

JSON and CSV schema v3 carry scene/run configuration plus engine version, Git commit and tracked
dirty state, build configuration, compiler, platform, architecture, OS, CPU model/logical CPUs,
Tracy state, GPU, driver information, and numeric Vulkan API/driver versions. Budget evaluation and
violations are retained with the raw frames. Schema v3 also records bounded chunk-mesh
invalidation-to-resident latency: each event begins at the monotonic dirty-region mark and ends only
when the exact requested mesh-stage revision is uploaded and published. It includes boundary
neighbors, reports a fixed 256-completion rolling median/P95/P99, and separates pending, coalesced,
completed, and abandoned intervals. "Visible" here means resident and eligible for the same frame's
draw list, not display scan-out. Keep the raw output with any conclusion.

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
9. Preserve output files; do not transcribe only a headline FPS number.
10. Add a new dated baseline below only when it remains useful for future decisions.

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
