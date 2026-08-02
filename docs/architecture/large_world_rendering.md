# Large-World Rendering, Visibility, and Residency

This document describes the maintained Renderer V2 large-world path. Exact world
addresses remain integer `BlockCoord` values. Render-facing positions are converted relative to a
`FloatingOrigin`; persistent spatial structures store double-precision world bounds and never
cache camera-relative coordinates across origin shifts.

## Visibility hierarchy

`renderer/visibility/visibility_hierarchy.*` owns the deterministic bounding-volume hierarchy used
by chunks and retained render objects. It supports incremental insert, update, erase, and ancestor
refit. A query accepts multiple views in one traversal, including the main camera, directional and
local shadow views, reflection views, and map views. Results contain per-view visibility and LOD
masks, node test counts, and culled-node counts.

LOD selection uses projected geometric error and screen height. Streaming priority is stable for a
given camera and object set, with distance, projected contribution, requested detail, and gameplay
importance represented explicitly. Multi-primitive prefab LOD chains use their shared object origin
as the distance reference so all primitives switch atomically.

Temporal occlusion results are keyed by the full `(view, object)` identity. Two adjacent occluded
results are required by default, while visibility is restored immediately. Camera cuts and floating
origin changes invalidate history.

## Hierarchical depth occlusion

`renderer/visibility/hierarchical_depth_occlusion.*` is the fixed-cost coarse occlusion path used by
high-volume vegetation patches. It projects explicit opaque occluder bounds into a low-resolution
depth buffer, stores the occluder's farthest depth for conservative coverage, and max-reduces the
buffer into a complete depth pyramid. A patch is rejected only when every hierarchy sample covering
its projected bounds is closer than the patch's nearest depth, including a configurable bias.

Near-plane intersections and partially covered bounds remain visible. Occlusion requires two
frames of agreement and resets after camera translation, camera rotation, or floating-origin cuts.
Fine object visibility still uses the shared scene hierarchy and hardware-instanced submission.

## Hybrid submission

The retained scene and vegetation paths perform coarse CPU hierarchy traversal and batch equal
mesh, material, render layer, and LOD records into hardware-instanced draws. Editable near terrain
uses a simpler per-chunk path because every chunk has distinct geometry. Far patches allocate from
shared vertex/index arenas and submit one multi-draw-indirect command per arena-buffer pair.
`firstInstance` indexes a per-frame patch-origin table, so multi-draw submission retains exact
floating-origin placement without baking camera-relative positions into persistent vertices.

Vulkan indirect indexed draws and counted indirect draws are part of the RHI, including
compute-to-indirect synchronization and a fallback to individual indirect commands when multi-draw
indirect is unavailable. The real Vulkan GPU test writes both command and count buffers in compute
and consumes them in graphics. Indirect barriers execute before dynamic rendering begins; indexed
state, scissor, descriptors, and push constants are bound before the indirect command is issued.

The Vulkan device advertises `multiDrawIndirect`, `drawIndirectFirstInstance`, and core Vulkan 1.2
`drawIndirectCount` independently. Far-patch batching requires both multi-draw and non-zero
indirect first-instance support because `firstInstance` selects patch data in the vertex shader;
otherwise it retains the direct-draw fallback. Counted draws are rejected when unsupported instead
of silently changing behavior.

## Far terrain

`renderer/terrain/far_terrain_clipmap.*` plans camera-snapped nested rings. Each patch has a stable
key, deterministic sampling positions, patch-local float geometry, a double-precision world origin,
geometric error, transition interval, and streaming priority. Adjacent levels overlap; the inner
256-metre exclusion leaves editable near chunks as the sole near-field owner.

`renderer/terrain/far_terrain_renderer.*` builds a bounded number of patches per frame, limits upload
bytes, retains old resident patches while replacements arrive, evicts patches outside the current
plan, and enforces a resident-byte budget. Its dedicated float-position vertex shader reuses the
production terrain fragment shader, voxel material table, normal/surface arrays, environment light,
clustered local lights, shadows, HDR targets, exposure, and tone mapping. Patch-local vertices avoid
precision loss; only the double world origin is converted relative to the active floating origin.

The surface sampler is reconstructed from the authoritative world seed using
`DeterministicTerrainGenerator::surface_height_at`. Ordinary loaded-chunk edits remain confined to
the near-field renderer; authored persistent far-landmark and edit propagation are future content
streaming extensions.

## General residency manager

`renderer/memory/streaming_residency.*` implements prioritized background loads, cancellation,
generation-based stale-result rejection, detail upgrades that retain the old resident resource,
render-thread upload budgets, fallback handles, pinned resources, deterministic eviction, and
deferred release callbacks. The effective budget is the smaller of the configured cap and a
configurable fraction of the driver-reported heap budget.

Load callbacks run on the configured job backend. Upload and release callbacks run from `process`
on the render thread. A Vulkan release callback must call the RHI release operation, which already
defers destruction until the resource's last submission has completed.

## Diagnostics

The renderer overlay and benchmark samples report:

* hierarchy nodes, tested nodes, and culled nodes
* direct and indirect draw counts
* planned, resident, visible, pending, and evicted far patches
* far-patch resident and per-frame upload bytes
* chunk queue, upload, residency, arena, and eviction counters
* device-local heap usage and budget from `VK_EXT_memory_budget`, when available
* CPU extraction/culling/build timing and per-pass GPU timing

## Validation and stress scenes

Use the deterministic headless tests for ordering and lifetime checks, then run the native Vulkan
benchmarks with validation enabled:

```bash
build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --scene flythrough --vulkan --warmup 60 --frames 300 --output flythrough.json
build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --scene churn --vulkan --warmup 60 --frames 300 --output churn.json
build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --scene large-coordinates --vulkan --warmup 60 --frames 300 --output large-coordinates.json
```

The `forest`, `caves`, `light-heavy`, and `resize-minimize` scenes cover the other representative
visibility policies. Benchmark output records the seed, backend, validation request, mesher, scene,
dimensions, warm-up length, and measured frame count for reproducibility.

## Current extension points

The hierarchy API already accepts underground, aerial, ocean, reflection, shadow, and map views.
The indirect RHI can consume command counts written by compute. Future content systems can migrate
individual repeated-prop families from CPU-packed instance batches to compute compaction without
changing render-pass ordering, pipeline layouts, or synchronization primitives.
