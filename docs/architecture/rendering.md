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

There is one colour path. World shading writes linear radiance into a transient `rgba16_sfloat`
`scene_hdr` target. SSAO is evaluated from scene depth and composited before transparencies, FXAA
stabilizes voxel/cutout edges, bloom filters HDR highlights, and `tone_map` combines bloom while
applying adapted exposure, colour grading, the tone curve, and the display transfer function. UI
then composites in display space. No world pass may write the presentable image;
`renderer_frame_graph_tests` enforces this.

**World shaders must not apply a transfer function.** `linear_to_srgb()` belongs in exactly two
places: `tone_map.frag`, which performs the single display encode, and `ui.frag`, which composites
after that encode has already happened. Adding it to a world shader double-encodes the image.

Exposure is expressed in stops and validated at the boundary. Automatic exposure uses a bounded,
frame-rate-independent exponential adaptation toward the scene-light estimate. Tone mapping
operators are
`none`, `reinhard`, `aces_approx` (default), and `khronos_pbr_neutral`.

`renderer_tone_mapping_tests` reads back the resolved image from a real device and asserts over the
pixels, so an exposure sign error, an unbound `scene_hdr` binding, or a resolve pass that ignores
its push constants fails in CI rather than reaching someone's screen.

### Pass attachments are per-pass

Each pass records into its own `vkCmdBeginRendering` block bound to the resources it declares it
writes, and load ops derive from whether a resource has already been written this frame. Anything
that reasons about attachments must do so per pass: colour format compatibility, whether depth is
bound, and which push constants a draw receives all differ between a world pass and a post-process
pass. Frame-wide assumptions about these were the main obstacle to running the HDR graph at all.

A pass may sample a graph resource by declaring `sampled_resources`, which maps a resource name onto
a named descriptor binding. Resources have no device handle, so the backend resolves the binding from
the resource pool every frame. Layouts that do this set `per_frame_descriptors`. The Vulkan backend
allocates graph descriptor sets per frame and per pass: a set bound by an earlier pass must not be
rewritten while its command remains pending, even when a later pass uses the same material layout.

Depth-only passes are first-class: a pass with a depth write and no colour write records normally
and cannot be silently skipped. Graph resources can also be storage images or storage buffers.
Graphics, compute, and transfer work retain declaration order, so compute can consume a depth
prepass and feed a later raster pass. Image declarations cover mip chains, arrays, cubemaps,
compressed BC1/BC3/BC5/BC7 formats, comparison sampling, and multiple colour attachments.

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

The maintained unified sequence contains four directional shadow cascades, two budgeted local
shadow maps, sky, opaque terrain, alpha-tested terrain, rich/static geometry, SSAO and AO
composition plus an immutable scene-depth copy, transparent/fluid geometry, debug, FXAA, bloom,
tone mapping, UI, and present.
Opaque, cutout, and transparent state are separate. The public frame is a dependency-validated,
bounded render graph schema rather than a parallel ad hoc frame path.

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

Terrain and retained model objects reuse the same composed object matrix, skin palette, morph data,
and alpha cutoff in shadow passes. This keeps animated, cutout, two-sided, and ordinary geometry
aligned between visible and shadow rendering.

## Materials, models, and textures

Renderer-facing material definitions use logical prototype IDs, domains, blend modes, validated
shader templates, texture bindings, scalar parameters, and color parameters. Mods and resource
packs operate through those declarations; they never receive raw Vulkan access.

Cooked model payloads carry the current versioned glTF-derived representation, including geometry,
attributes, skinning, animation/morph data, materials, texture references, and extension-derived
runtime data supported by the asset pipeline. Texture delivery includes the maintained RGBA and
Basis/KTX2 paths. Do not duplicate the full import matrix here; [the asset pipeline guide](../asset_pipeline.md)
is the contributor-facing source of truth.

The standard surface record carries base colour, normal, metallic/roughness, occlusion, emissive,
alpha cutoff, double-sided/unlit flags, vertex colour, two UV sets, per-texture UV transforms and
sampler state, and stable fallback layers. Terrain uses the same Cook-Torrance direct response,
metallic/roughness, occlusion, emissive, alpha, two-sided, and unlit semantics with six independent
face texture ranges. Alpha-tested surfaces discard by material cutoff. Transparent/fluid surfaces
blend without depth writes. Runtime material overrides update GPU tables without changing the
pipeline ABI.

The maintained scene target is linear `rgba16_sfloat`. The final output image is RGBA8 UNORM
containing shader-encoded sRGB values, so native presentation prefers an RGBA8 or BGRA8 UNORM
swapchain with `SRGB_NONLINEAR` presentation colour space. Using an sRGB storage format for that
final blit would encode the already encoded scene a second time.

The surface-material storage buffer has an explicit 224-byte CPU/GPU record contract. Its final
flag word and padding are exposed to GLSL as one aligned `uvec4`; splitting that tail into a scalar
and `uvec3` changes the std430 array stride and causes later materials to read neighboring records.
The separate voxel-material buffer uses a 112-byte std430 record: two `uvec4` lanes each for six
face-range starts and counts, one flags/padding lane, base color, and surface parameters.

## Lighting, shadows, and grounding

Terrain and imported surfaces share Cook-Torrance GGX direct lighting for the sun, point lights,
and spotlights. Local lights are CPU-binned into deterministic 16×16 screen tiles, uploaded as a
bounded storage-buffer grid, and evaluated only by covered fragments. The default limits are 1,024
submitted lights and 32 lights per tile; overflow is counted.

Environment lighting uses a mipmapped cubemap. A high mip supplies diffuse sky irradiance,
roughness selects the prefiltered specular mip, and a split-sum BRDF integration fit supplies the
roughness/NdotV response. Intensity and Y rotation are frame controls. Material AO and the
authoritative voxel light field attenuate the environment term; zero-light caves therefore do not
receive a constant bright ambient value.

Directional shadows use four 2048² cascades with practical split blending, texel-snapped
projections, slope/constant bias, 3×3 PCF, and distance fade. Terrain, cutouts, static/rich meshes,
skinning, morph targets, and two-sided vegetation cast through depth-only passes.

Two 1024² local spotlight maps form the default local-shadow budget. Selection is stable and scores
projected contribution (radius and distance), intensity, colour, gameplay importance, and whether
the light or nearby geometry revision changed. Selected slots are written into tiled-light records
and sampled with comparison PCF. Point lights remain fully supported for direct lighting; their
omnidirectional shadow-map path is not yet enabled.

Debug modes display base colour, normal, roughness, metallic, AO, emissive, cascade coverage, and
local tile occupancy through `Renderer::set_lighting_debug_view`.

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
upload segments, validated vertex ABIs, clipping, and explicit counters. The UI atlas provides a
white primitive layer and a deterministic SDF layer built from the packaged Noto Sans font. Text
submission fails explicitly when that production font is unavailable; there is no bitmap fallback.
Game UI submits presentation data, not backend commands.

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

`heartstead_render_benchmark` supplies deterministic static and stress scenes, including the
128-local-light `light-heavy` settlement workload and the `terrain-materials` material/state/slope
preview, excludes warm-up, settles initial residency, records complete per-frame counters, and
exports versioned JSON or CSV. Use an optimized build and preserve run configuration when comparing
changes. The maintained methodology and historical baseline are in
[Renderer benchmarks](../performance/renderer_benchmarks.md).

## Extension rules

New rendering work should enter through engine-owned resources, extracted scene data, materials,
passes, debug draw, or validated shader extension points. Do not add Vulkan types to gameplay,
mods, saves, replication, or public world state. Add a backend capability and headless validation
before relying on a new native operation.
