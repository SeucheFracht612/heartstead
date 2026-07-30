# Rendering architecture

Rendering is a presentation boundary below game, simulation, persistence, networking, and mod
meaning. Higher layers provide renderer-neutral resources and extracted scene data. Only backend
implementations own Vulkan objects, synchronization, swapchains, descriptor sets, pipelines, or
GPU allocation details.

## Backends

Heartstead has two maintained renderer backends:

- **Headless:** deterministic validation for tests, tools, CI, and non-presenting applications. It
  validates frame plans, resources, descriptors, draw bounds, camera data, synchronization intent,
  clipping, and lifetime without pretending to execute GPU work.
- **Vulkan:** the native Linux/X11 path. It owns instance/device/queue selection, X11 surface,
  swapchain, offscreen color/depth targets, frame and upload contexts, command recording,
  synchronization, descriptors, resources, pipelines, timestamps, and presentation.

Backend capabilities are explicit. Unsupported operations fail before partial submission rather
than being silently approximated.

## Ownership and threading

The renderer and backend are owner-thread objects. Vulkan calls, resource publication, descriptor
mutation, frame-plan construction, and teardown occur on that owner. Worker threads receive only
immutable, bounded snapshots and publish typed results through bounded queues.

Released GPU resources are retired by queue-submission serial and destroyed only after the backend
reports completion. Shutdown and presentation-mode teardown perform an owner-thread idle barrier
before releasing resources still referenced by in-flight descriptors or command buffers.

## Frame lifecycle

A normal native frame:

1. polls completed frame/upload work and deferred releases;
2. accepts bounded completed asset/mesh work;
3. updates presentation resources and visibility;
4. extracts camera-relative draws and debug/UI batches;
5. acquires a frame context and optional swapchain image;
6. records one unified command buffer;
7. submits once, presents once when native, and records timings/statistics;
8. advances frame and submission generations.

The maintained unified sequence contains sky, opaque terrain, alpha-tested terrain, rich/static
geometry, transparent/fluid geometry, debug, UI, and present phases. Opaque, cutout, and transparent
state are separate. The current renderer intentionally exposes a bounded frame schema rather than a
general-purpose render graph.

Resize/minimize events recreate or suspend presentation targets without discarding authoritative
state or retained chunk meshes. A zero drawable extent does not busy-loop submission.

## Coordinates and camera

Authoritative positions remain signed integer anchors. Rendering subtracts a camera-relative origin
before converting to floating point, preserving precision at large world coordinates. Matrices use
the engine's declared column-major, column-vector, Vulkan zero-to-one depth convention.

Terrain/static push constants carry view-projection, camera-relative origin, sun/ambient, and fog
data. Screen-space UI uses a top-left pixel origin and backend-consistent clipping/scissor rules.

## Chunk rendering

`ChunkRenderSystem` owns asynchronous terrain-mesh scheduling, retained GPU residency, visibility,
and budgets. The owner thread captures a bounded center-plus-neighbor-halo snapshot and a compact
render table. Workers never query live `WorldState`, `ChunkDatabase`, voxel chunks, or the prototype
registry.

Every result carries content, neighbor, render-table, load-generation, and request revisions. The
owner rejects stale or superseded work before upload preparation and again before publication.
Rebuilds keep the previous resident mesh visible; rapid requests coalesce or cancel when possible.

The readable reference mesher handles the complete supported voxel/rich-model path. The greedy
full-cube mesher merges faces only when material, voxel type, phase, light, state, and relevant flags
match. Mesh sections preserve material/phase ranges so visibility emits bounded indexed draws.

Resident terrain uses explicit byte and distance budgets. Eviction prefers far, nonvisible, and
least-recently-visible meshes. Memory-pressure suppression prevents immediate rebuild thrash.
Statistics expose residency, pending work, uploads, evictions, arena usage/fragmentation, pool
reuse, stale results, and overflow.

See [Chunk meshing](chunk_meshing.md) for voxel geometry rules.

## Dynamic and rich presentation

The renderer accepts retained or frame-extracted data for rich/static models, animated entities,
particles, debug geometry, and UI. Fixed capacities and bounded upload regions make overflow visible
in statistics rather than allowing unbounded per-frame allocation.

Animation, model instances, particles, and UI retain their own engine-level handles and generation
checks. Presentation handles can be discarded and rebuilt without changing entity save identity or
world state.

## Materials, models, and textures

Renderer-facing material definitions use logical prototype IDs, domains, blend modes, validated
shader templates, texture bindings, scalar parameters, and color parameters. Mods and resource
packs operate through those declarations; they never receive raw Vulkan access.

Cooked model payloads carry the current versioned glTF-derived representation, including geometry,
attributes, skinning, animation/morph data, materials, texture references, and extension-derived
runtime data supported by the asset pipeline. Texture delivery includes the maintained RGBA and
Basis/KTX2 paths. Do not duplicate the full import matrix here; [the asset pipeline guide](../asset_pipeline.md)
is the contributor-facing source of truth.

Alpha-tested surfaces discard by material cutoff. Transparent/fluid surfaces blend without depth
writes. Lighting is evaluated in linear space and encoded for the current scene target.

## Shader and pipeline lifecycle

Built-in runtime rendering loads validated SPIR-V; it does not compile shader source during normal
frames. CMake can regenerate built-in SPIR-V with external glslang tools, while checked-in payloads
provide a deterministic fallback.

`ShaderProgramHandle` and pipeline-cache entries are generation-safe. Development hot reload
creates and validates complete replacements before changing resident programs. A failed reload
keeps the last valid modules and pipelines. Common pipelines are prewarmed, then normal frames do
not create surprise pipelines. Dependent pipeline replacement is transactional.

The general asset shader compiler remains a controlled cook/validation boundary. Production
Slang/HLSL compilation is unavailable until a real compiler backend is linked; validated SPIR-V
passthrough is supported.

## Debug and UI

Debug lines/text and UI triangles are renderer-neutral batches with fixed capacities, rotating
upload segments, validated vertex ABIs, clipping, and explicit counters. The built-in UI atlas
provides a white primitive layer and deterministic fallback diagnostic font. Game UI should submit
presentation data, not backend commands.

Mandatory engine diagnostics include coordinates, chunk bounds/revisions, render/collision bounds,
meshing/residency, controller geometry, rooms, lights, processes, network state, and relevant
resource statistics.

## Failure behavior

- Invalid resources, descriptors, draw ranges, pass associations, or camera values fail validation.
- Unsupported Vulkan frame plans return an explicit error before submission.
- Asset/mesh upload publishes only after every required write succeeds.
- Stale asynchronous results are discarded without replacing valid resident resources.
- Shader/pipeline reload failure retains the last valid generation.
- Device/surface/presentation failure is surfaced to the application; it does not mutate gameplay
  or save state.
- Capacity overflow and budget pressure are reported through statistics and deterministic drop,
  defer, or eviction policy.

## Performance and benchmarks

`Renderer::stats()` distinguishes CPU synchronization, extraction, culling, draw-list/command
construction, snapshot capture, meshing, upload preparation/copy, backend recording, GPU waits, GPU
passes, residency, and overflow. Vulkan timestamp results retain their source frame because GPU
measurements arrive later than CPU frame data.

`heartstead_render_benchmark` supplies deterministic static and stress scenes, excludes warm-up,
settles initial residency, records complete per-frame counters, and exports versioned JSON or CSV.
Use an optimized build and preserve run configuration when comparing changes. The maintained
methodology and historical baseline are in
[Renderer benchmarks](../performance/renderer_benchmarks.md).

## Extension rules

New rendering work should enter through engine-owned resources, extracted scene data, materials,
passes, debug draw, or validated shader extension points. Do not add Vulkan types to gameplay,
mods, saves, replication, or public world state. Add a backend capability and headless validation
before relying on a new native operation.
