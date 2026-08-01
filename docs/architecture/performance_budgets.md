# Renderer Performance Budgets

Status: authoritative baseline, not a hardware guarantee.

Release benchmarks report median/p95/p99, 1%/0.1% lows, upload volume, memory, visibility, draws,
triangles, instances, lights, and per-pass GPU timing. Debug one-frame captures are regression checks,
not shipping performance numbers.

High defaults include 16 visible terrain chunks horizontally with mesh/resident/load hysteresis,
8 MiB near and 8 MiB far-terrain uploads per frame, 512 MiB generic residency, 1,024 local lights,
32 lights per tile, two local shadow maps, 2048 directional shadow resolution, and 320 m shadow range.

New systems expose queue depth, dropped work, uploaded/resident bytes, visible/culled counts, and CPU/
GPU time. Exhaustion degrades deterministically through LOD, selection, or deferral rather than stalls
or unbounded allocation.

See [Renderer benchmarks](../performance/renderer_benchmarks.md) for workloads, timing semantics,
comparison rules, and dated measurements.
