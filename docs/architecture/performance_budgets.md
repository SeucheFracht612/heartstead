# Renderer performance budgets

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

High defaults include 16 visible terrain chunks horizontally with mesh/resident/load hysteresis,
8 MiB near and 8 MiB far-terrain uploads per frame, 512 MiB generic residency, 1,024 local lights,
32 lights per tile, two local shadow maps, 2048 directional shadow resolution, and 320 m shadow range.

New systems expose queue depth, dropped work, uploaded/resident bytes, visible/culled counts, and CPU/
GPU time. Exhaustion degrades deterministically through LOD, selection, or deferral rather than stalls
or unbounded allocation.

See [Renderer benchmarks](../performance/renderer_benchmarks.md) for workloads, timing semantics,
comparison rules, command usage, and dated measurements. See the
[voxel optimization roadmap](../performance/voxel_optimization_roadmap.md) for the broader staged
budget system and calibration policy.
