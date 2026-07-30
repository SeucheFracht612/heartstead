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

## Workloads

The deterministic catalog includes static and stress coverage for:

- flat terrain;
- mountains;
- caves;
- checkerboard/worst-case face separation;
- cross-plane forest/foliage;
- rapid edits and replacement meshes;
- high-speed flythrough/streaming;
- load/unload churn;
- large world coordinates;
- resize/minimize behavior.

Integer/hash generation and frame-indexed stress schedules make runs reproducible from scene, seed,
radius, and frame configuration. Before measurement, the runner settles the initially loaded chunks
to resident meshes so streaming/edit tests measure replacement and churn rather than accidental
initial starvation.

## Timing semantics

Warm-up frames are excluded. Output records per-frame renderer counters plus summary statistics,
including median, p95, p99, maximum frame time, 1% and 0.1% low FPS, and CPU/GPU subsystem timing.
CPU zones distinguish synchronization, extraction, culling, draw-list and command construction,
snapshot capture, meshing, upload preparation/copy, backend recording, and blocking waits.

Vulkan timestamps are asynchronous. Every GPU result carries the source frame and result latency;
do not align a delayed GPU sample with the CPU frame that happened to receive it. The headless
backend reports GPU timing unavailable while preserving the same CPU/counter schema.

JSON and CSV carry a versioned schema and complete run configuration: scene, seed, backend, mesher,
resolution, radius, warm-up and measured frames, frame cap, and validation request. Keep the raw
output with any conclusion.

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

## Publishing a new baseline

Append a dated section with commit SHA, dirty-tree state, compiler, build preset, CPU/GPU, RAM, OS,
kernel, driver, display/resolution, backend, validation state, command lines, repeat count, and links
to raw output artifacts. Explain the decision the data supports and any source of noise. Never
replace an old machine's values with new values under the same label.
