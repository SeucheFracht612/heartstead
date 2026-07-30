# Rendering architecture

Rendering is a presentation boundary below game, simulation, persistence, networking, and mod
meaning. Higher layers provide renderer-neutral resources and extracted scene data. Only backend
implementations own Vulkan objects, synchronization, swapchains, descriptor sets, pipelines, or
GPU allocation details.

## Vulkan baseline

The Vulkan backend targets **Vulkan 1.3 core** and requires the `dynamicRendering` feature. A
physical device that does not report API version 1.3 with dynamic rendering is skipped during
selection, and device creation fails rather than falling back to a legacy path.

There are no `VkRenderPass` or `VkFramebuffer` objects. Pipelines declare the attachment formats
they are compatible with through `VkPipelineRenderingCreateInfo`, and the frame supplies the actual
image views at `vkCmdBeginRendering` time. This is what allows scene passes to target an HDR image
while the tone mapping pass targets the swapchain, without pipelines being coupled to a shared
render pass object.

Synchronization still uses `vkCmdPipelineBarrier`. Converting to `synchronization2` is deliberate
follow-up work and is not required for graph-driven attachments.

## Colour pipeline

`FrameBuilder` builds one of two graphs, selected by `FrameColorPipeline`:

- **`linear_hdr`** (production target): world shading writes linear radiance into a transient
  `rgba16_sfloat` `scene_hdr` target. A single `tone_map` post-process pass applies exposure, the
  tone curve, and the display transfer function, writing the swapchain image. UI composites on top
  in display space. No world pass may write the presentable image; `renderer_frame_graph_tests`
  enforces this.
- **`legacy_ldr`** (being retired): world shaders each apply their own `linear_to_srgb()` and write
  straight into the swapchain. This remains the default only until the Vulkan backend executes the
  HDR graph, at which point it and the per-shader gamma encoding are deleted together.

Exposure is expressed in stops and validated at the boundary. Tone mapping operators are
`none`, `reinhard`, `aces_approx` (default), and `khronos_pbr_neutral`.

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
Terrain materials carry six independent face texture ranges in west/east/bottom/top/north/south
order. Each range contains a primary authored tile and zero or more authored variants. A stable
chunk-coordinate seed plus the fragment's owning local cell and face selects the range entry, so
variation remains fixed across camera movement and floating-origin rebases without entering world
state or networking.

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

Each retained model object carries an entity transform plus an optional model-local matrix. Static
objects leave the latter at identity. Rigid-node animation supplies the evaluated matrix of the
primitive's owning glTF node; skinned animation supplies a joint palette evaluated from that same
node pose. Entity visuals may prepend a uniform presentation scale to either branch. Culling and
every maintained surface layer compose the model-local matrix after the interpolated entity
transform, so opaque, alpha-tested, and transparent results stay aligned.

The current frame sequence has a depth attachment but no shadow-map pass. `cast_shadow` is retained
as presentation intent only; a future shadow implementation must reuse the same composed object
matrix and skin palette.

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

The maintained scene target is RGBA8 UNORM containing shader-encoded sRGB values. Native
presentation therefore prefers an RGBA8 or BGRA8 UNORM swapchain with
`SRGB_NONLINEAR` presentation color space. Using an sRGB storage format for that final blit would
encode the already encoded scene a second time, washing out midtones and reducing visible texture
contrast. Sky, terrain, static/model, debug, and UI fragment paths all follow the same explicit
output-transfer contract.

The surface-material storage buffer has an explicit 224-byte CPU/GPU record contract. Its final
flag word and padding are exposed to GLSL as one aligned `uvec4`; splitting that tail into a scalar
and `uvec3` changes the std430 array stride and causes later materials to read neighboring records.
The separate voxel-material buffer uses a 112-byte std430 record: two `uvec4` lanes each for six
face-range starts and counts, one flags/padding lane, base color, and surface parameters.

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
