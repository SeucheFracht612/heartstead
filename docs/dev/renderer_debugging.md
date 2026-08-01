# Renderer Debugging and Diagnostics

`renderer::format_renderer_stats()` reports CPU/GPU time, GPU phases, draws/triangles/instances,
visible/culled chunks and objects, lights/shadows, uploads, streaming/device memory, descriptor
bindings, samplers, pipeline layouts, graph lifetime counts, shader/pipeline errors, particles, and UI.

Lighting debug views cover material channels, UVs, tangents, vertex color, mip/texel density, texture
residency, LOD, bounds, skeletons, skin weights, cascades, local-light tiles, and overdraw. Streaming
and visibility expose queue, LOD, occlusion, and memory-pressure snapshots.

Use validation in normal Vulkan debug runs. A normal-operation validation error is a renderer bug.
Debug labels name passes/resources. Timestamp results arrive with latency and identify their measured
frame. Shader reload failures retain the last valid program; sealed-cache variant misses are errors.

See [Visual regression](visual_regression.md) and [Benchmarks](../performance/renderer_benchmarks.md).

Run a bounded native diagnostic session with:

```bash
./build/default-debug-werror/apps/dev_game/heartstead_dev_game \
  --native-frames 1200 --no-save
```

Press `F3` for the renderer/runtime diagnostics overlay. Use Asset Lab `--debug VIEW` for isolated
material, shadow, LOD, residency, skeleton, and overdraw inspection; run
`heartstead_asset_lab --list` for the exact view names. Use the benchmark `--vulkan` path when a
repeatable scene and exported timing record are required.
