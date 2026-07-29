# Rendering Architecture

Rendering is an engine-owned system behind a small RHI boundary.

Implemented foundation:

- `RenderBackend`
  - names supported backend slots: `headless` and `vulkan`
  - lets tools and samples query whether a backend is currently available

- `IRenderDevice`
  - owns backend-specific frame execution
  - exposes `RenderDeviceCapabilities`
  - exposes current output extent, completed frame count, latest submission serial, and completed
    submission serial
  - devices returned by the public `rhi::create_render_device` factory bind every mutating call to
    the creating thread and return `renderer.render_device_wrong_thread` before touching backend
    state; read-only capability/stat getters remain callable without a fallible thread check
  - validates resize and frame output extents
  - returns per-frame stats for smoke tests and debug tooling
  - executes `RenderFrameSubmission` records that combine a validated `RenderFramePlan`, camera
    data, and pass-associated indexed draw commands in one frame-level contract
  - owns opaque renderer resource handles for uploaded buffers and sampled images
  - can upload renderer-neutral CPU data, such as chunk mesh vertices, indices, and small RGBA8
    sampled images, without exposing backend handles to world/chunk/material systems
  - can create host-visible or device-local buffers and batch validated writes into explicit byte
    ranges, allowing renderer-owned arenas without exposing backend allocation objects
  - can create shader modules from validated SPIR-V words without exposing backend shader handles
  - can create compute pipeline objects from shader modules and bound material pipeline layouts
    without exposing backend pipeline handles
  - can create graphics pipeline objects from vertex/fragment shader modules and bound material
    pipeline layouts without exposing backend pipeline handles
  - can write descriptor bindings for material scalar/color uniform buffers, storage buffers, and
    sampled texture arrays with shared samplers without exposing backend descriptor set handles
  - rejects empty/unknown descriptor shader-stage masks; `ShaderManager` also checks those masks
    against stages present in the resident program interface
  - requires every required descriptor before a draw, rejects duplicate targets within one
    descriptor-write batch, and leaves descriptor state unchanged when batch validation fails
  - retains the old material-based `bind_mesh_draws()` API only as a deprecated compatibility path;
    it is isolated to the legacy renderer smoke sample and RHI regression tests, while every visible
    application/front-end path uses explicit pipeline handles through `execute_frame()`
  - builds renderer-neutral frame execution plans so Vulkan barriers and image-layout transitions
    are derived from the public RHI contract rather than hidden backend inference
  - can structurally bind material pipeline layouts, including shader template and descriptor slots,
    without exposing Vulkan pipeline layouts or descriptor sets through the public RHI

- `RenderDeviceDesc`
  - selects backend, application name, initial extent, present mode, validation, and an optional
    platform-native window handle
  - lets the Vulkan backend create a private surface without exposing Vulkan or Xlib types through
    the public RHI

- `RenderDeviceCapabilities`
  - reports backend, maximum extent, present support, validation support, debug marker support,
    shader-module support, pipeline layout support, compute-pipeline support, graphics-pipeline
    support, descriptor-write support, buffer upload support, image upload support, draw binding
    support, and whether the device is headless
  - gives future Vulkan setup, diagnostics, and tests a stable capability query without exposing
    backend handles

- `RenderBackendCapabilities`
  - reports backend availability before a device is created
  - describes present, validation, debug marker, shader-module, pipeline-layout,
    compute-pipeline, graphics-pipeline, descriptor-write, buffer-upload, image-upload,
    draw-binding, window-surface, GPU-device, headless, frames-in-flight, and graphics API
    requirements
  - lets tools inspect the Vulkan contract even on machines where the optional backend is
    unavailable

- `RenderFramePlan`
  - declares named render resources and ordered render passes
  - validates resource names, pass names, extents, duplicate declarations, read-before-write
    hazards, duplicate pass resource references, and unambiguous present-pass rules
  - compiles validated plans into a renderer-neutral execution plan with ordered pass names,
    per-resource access records, and explicit resource dependency edges for write/read,
    write/write, read/write, and present transitions
  - derives renderer-neutral transition records for `undefined`, `external`, `shader_read`,
    color/depth attachment writes, and `present`, and defines transfer source/destination states for
    backend copy operations without exposing Vulkan enums
  - models clear, world, post-process, UI, debug, and present pass kinds without exposing Vulkan
    objects
  - can be submitted to an `IRenderDevice` for smoke execution, with backend frame stats preserving
    the validated pass, resource-use, dependency, transition, planned synchronization-barrier, and
    backend-submitted synchronization-barrier counts
  - is a broader renderer-neutral contract than every backend is required to execute: headless runs
    arbitrary validated plans, while Vulkan explicitly rejects shapes outside its supported clear
    and unified-frame schemas before submission
  - gives shader-pack extensions and debug tooling one validated planning vocabulary without
    implying that declaration alone implements a Vulkan pass

- Renderer-neutral chunk mesh data
  - `ChunkMesher` extracts CPU-side terrain surface meshes outside the renderer backend
  - `GpuTerrainVertex` defines a stable 24-byte compact GPU ABI with asserted field offsets and
    explicit integer/normalized shader locations; position and UV use fixed-point encoding, while
    normal, lighting, ambient occlusion, corner, and flags use byte fields
  - the renderer uploads converted vertex data and selects 16-bit indices whenever the complete
    mesh fits, retaining a 32-bit fallback for pathological meshes; index type is explicit in every
    draw command
  - each terrain draw carries a camera-relative chunk origin while the exact world anchor remains in
    `FloatingOrigin`

- Retained renderer front end
  - `Renderer` owns the `IRenderDevice`, shared terrain pipeline, `ChunkGpuCache`,
    `ChunkRenderSystem`, and `FrameBuilder`; `WorldState` remains owned by its caller
  - initial synchronization discovers chunks that predate renderer startup, while explicit
    `ChunkStreamer` load and generation-aware eviction reports can be forwarded without an event bus
  - each `ChunkGpuEntry` records its exact `ChunkIdentity`, resident content and block-render-table
    revisions, complete neighbor dependency stamps, generation-safe vertex/index arena ranges,
    counts, local render bounds, state, and resident bytes
  - immutable snapshot, asynchronous mesh-result, and owner-thread upload queues retain work across
    frames, prioritize visible/missing/nearby chunks, and enforce per-frame snapshot, scheduling,
    result-drain, and upload-byte budgets
  - accepted chunk uploads are suballocated from growing device-local vertex and index arenas and
    copied in one batch per frame budget; replacements become resident only after all writes succeed
  - the prior mesh remains drawable while a rebuild is queued or an allocation/upload fails
  - empty chunks become valid resident entries without allocating vertex or index buffers
  - mesh dirty regions have one renderer consumer; mesh dirtiness clears only after the requested
    identity, content revision, render-table revision, and all neighbor dependencies remain current
    following a successful upload
  - `WorldRenderDistances` separates simulation, loaded, mesh, GPU-resident, and visible cylinders,
    each with independent horizontal and vertical radii; validation keeps inner tiers within outer
    tiers
  - distance rejection runs before six-plane frustum testing of camera-relative chunk AABBs,
    including rich-model bounds, before opaque terrain commands are built
  - GPU meshes leaving the residency cylinder plus hysteresis are retired without removing their
    renderer-owned loaded-chunk records, so returning chunks can rebuild without world reload churn
  - a configurable terrain byte budget evicts far, nonvisible, and least-recently-visible meshes
    first; memory-pressure suppression prevents evicted records from immediately rebuilding until
    capacity exists or their camera priority exceeds a current resident
  - evictions and replacements return ranges through serial-tagged retirement and collect them
    only after the RHI's completed submission serial reaches the last submission that could have
    referenced the old range
  - debug statistics expose resident/empty counts and bytes, arena capacity/usage/free space and
    fragmentation, the residency budget and distance/memory-pressure evictions, pending mesh/upload
    work, batched writes, visible/distance-culled/frustum-culled chunks, draws, pipeline binds,
    vertices, and indices

- Runtime render assets and materials
  - `ShaderManager`, `TextureManager`, `SamplerCache`, `MaterialRuntimeCache`, and `PipelineCache`
    are renderer-owned; chunks retain only mesh allocations and never own descriptors or pipelines
  - texture handles are generation-safe and missing/stale references resolve to a deterministic
    checkerboard error texture; white, black, and flat-normal fallback textures are always resident
  - RGBA8 texture arrays carry explicit linear or sRGB formats and complete CPU-generated mip
    chains; sRGB downsampling converts color channels through linear space
  - sampler descriptors are cached, so equivalent materials share one GPU sampler
  - stable terrain texture-layer indices feed a contiguous 48-byte `GpuVoxelMaterial` table; that
    storage buffer can change color, texture layers, flags, emissive strength, or roughness without
    remeshing chunks
  - the shared terrain pipeline samples the array using integer voxel-type data from the real chunk
    vertex ABI and indexes the material table; its pipeline binds are counted per frame

- Retained dynamic render scene
  - gameplay submits generation-safe object and light proxies through `RenderScene`; removal is
    explicit and stale update/removal handles are rejected
  - object proxies retain exact integer world anchors, previous/current local transforms, mesh and
    material asset handles, conservative local bounds, render layer, color, sprite/atlas frame,
    and optional parent
  - extraction interpolates only local floating-point transforms, resolves parent/assembly
    hierarchies from an exact root anchor, converts roots relative to the camera floating origin,
    and frustum-culls transformed bounds
  - compatible visible objects are grouped by layer, material, and mesh into stable instance
    batches; hidden, visible, culled, retained, and batch counts are reported
  - directional and point lights are retained independently and extracted into camera-relative
    frame data without searching gameplay/entity databases
  - `MeshManager` validates an explicit 56-byte unified static/skinned-mesh vertex ABI, caches
    shared assets by id,
    suballocates device-local vertex/index arenas, retires released ranges by submission serial, and
    resolves stale/missing handles to a visible manager-owned error cube
  - `SceneRenderSystem` flattens batches into a 96-byte instance ABI (camera-relative matrix,
    color, layer metadata), rotates through buffered storage-table segments, and emits one indexed
    draw per compatible mesh/material/layer batch using `first_instance`
  - opaque, cutout, and transparent object pipelines are prewarmed; transparent instances and
    batches use stable back-to-front ordering, depth testing, blending, and disabled depth writes;
    terrain sections and static-object batches are sorted independently before their two lists are
    concatenated, so there is not yet one global cross-category transparency sort
  - instance capacity is a fixed configuration budget: overflow is delayed/dropped visibly in
    statistics instead of growing frame memory without bound
  - the bounded CPU particle system and retained billboard bridge are described in
    [particles.md](particles.md); their stress scene uses the same instance ABI and publishes
    simulation/presentation timings separately

- Gameplay rendering tools
  - `DebugRenderer` accepts thread-safe one-frame or timed lines, rays, axes, AABBs, oriented
    boxes, spheres, and camera-relative text-label bridge records; depth-tested and overlay lines
    use separately prewarmed line-list pipelines
  - debug geometry uses an asserted 28-byte vertex ABI, fixed line/text capacities, rotating
    device-local buffer segments, batched writes, and visible overflow/upload/draw counters
  - `UiRenderer` accepts owner-thread screen-space triangle batches, quads, and text, batches a
    complete text string into one indexed draw, and emits only renderer-neutral UI commands
  - UI vertices use an asserted 36-byte ABI with explicit position, UV, color, and integer atlas
    layer locations; the built-in two-layer sRGB atlas provides a white primitive layer and a
    deterministic 5x7 fallback diagnostic font
  - clipping is a per-draw RHI scissor validated against the current framebuffer before either
    backend executes it; the Vulkan backend programs the dynamic scissor immediately before the
    indexed draw
  - UI geometry has fixed vertex/index capacities and rotating device-local segments. Statistics
    expose vertices, glyphs, clipped draws, upload bytes, and overflow instead of allowing
    unbounded frame allocations
  - debug and UI shaders participate in development hot reload while preserving the last valid
    program and pipeline when validation or replacement fails

- Camera and shader constants
  - `Mat4f` uses column-major storage, column vectors, and Vulkan's zero-to-one depth convention
  - `RenderCamera` owns the local position, yaw/pitch perspective, resize-dependent aspect ratio,
    and view-projection composition
  - terrain uses a 128-byte vertex/fragment push-constant block: a 64-byte view-projection matrix,
    a 16-byte camera-relative chunk origin, and three 16-byte environment lanes carrying the sun,
    ambient light, and distance-fog parameters
  - terrain/static lighting is evaluated in linear space and those shaders explicitly encode into
    the RGBA8-unorm scene target; alpha-tested surfaces discard below the material cutoff, and
    transparent/fluid surfaces blend without writing depth
  - the unified terrain frame records sky, opaque, alpha-tested, rich/static, transparent/fluid,
    debug, UI, and present phases; opaque/cutout/transparent terrain use separately prewarmed state

- `MaterialDefinition` and `MaterialRegistry`
  - describe renderer-facing material data with prototype ids, domains, blend modes, shader
    templates, texture bindings, scalar parameters, and color parameters
  - validate virtual paths, finite numeric values, color ranges, and duplicate parameter names
  - can be built from `material` prototypes loaded through the normal mod content pipeline
  - validate declared shader templates and texture bindings against active asset catalog records
  - can produce structural RHI pipeline layouts from shader template, texture, scalar, and color
    declarations
  - keep material and shader-pack data above backend handles so mods and resource packs can use
    declared templates instead of raw Vulkan access

- `ShaderCompiler`
  - consumes active shader assets from the asset catalog, after mod/resource-pack override rules
    have selected the winning source
  - infers source language (`slang`, `hlsl`, or SPIR-V passthrough) and role (`template`,
    `vertex`, `fragment`, `compute`, or `library`) from the virtual source path
  - writes a deterministic development shader manifest and compiled payload wrappers
  - supports a production profile for validated SPIR-V passthrough payloads
  - reports explicit diagnostics when production Slang/HLSL compilation is requested before a real
    compiler backend is linked
  - exposes structured inspection for compiled shader records and compile results through the
    engine inspector and `heartstead_shader_compiler --inspect`
  - gives shader packs a controlled validation/cook boundary before real Slang-to-SPIR-V
    integration exists

- Headless render backend
  - compiles everywhere
  - validates renderer lifetime, resize, clear color, frame indexing, present intent, and the
    default clear/present frame plan
  - executes arbitrary validated frame plans deterministically for tests and tools
  - reports deterministic capabilities for present, validation, debug markers, buffer upload, and
    pipeline layout binding, draw binding, and max extent
  - records uploaded buffer and image handles without pretending to own GPU memory
  - validates material pipeline layouts and descriptor slot declarations without pretending to own
    GPU pipeline layouts
  - validates uniform descriptor writes against bound material layouts, uploaded uniform buffers,
    and byte ranges
  - validates sampled texture descriptor writes against bound material layouts and uploaded image
    resources
  - validates mesh draw bindings against uploaded buffer usage, material prototype ids, and
    material graphics pipeline availability without submitting GPU commands
  - validates unified submissions, pass association, explicit pipeline/buffer handles, index
    ranges, instance counts, finite camera data, push-constant coverage, and color/depth targets
  - gives tests and CI a renderer path before OS windows or Vulkan are required

- Vulkan backend boundary
  - exists as an explicit backend factory
  - is compiled when CMake finds the Vulkan SDK/loader and `HEARTSTEAD_ENABLE_VULKAN` is enabled
  - creates a Vulkan instance, selects a graphics-and-present-capable physical device, and creates
    the logical device and queue
  - can create and own an X11 `VkSurfaceKHR` from a platform-native window handle when the optional
    X11 backend is compiled
  - owns a private command pool, command buffer, fence, synchronization semaphores, offscreen color
    and depth targets, and an optional swapchain
  - selects a supported depth attachment format, preferring `D32_SFLOAT`, and recreates color,
    depth, and swapchain targets when the output extent changes
  - can allocate host-visible Vulkan buffers and copy renderer-neutral upload bytes into them behind
    opaque RHI handles
  - creates device-local arena buffers with transfer-destination usage and batches subrange copies
    through a persistently mapped 32 MiB staging ring whose ranges carry submission serials; an
    oversized or temporarily full batch uses a dedicated fallback staging allocation
  - owns two configurable frame contexts by default, each with a command pool, command buffer,
    fence, acquire semaphore, framebuffer, and private offscreen color/depth target; normal frame
    submission waits only when reusing a still-busy context
  - owns matching asynchronous upload contexts, so a staged buffer batch returns after queue
    submission and staging ranges or fallback buffers remain alive until their serial completes
  - tracks a monotonically increasing queue-submission serial across frame and upload submissions,
    exposes both the latest and completed serial through the RHI, and defers released Vulkan
    buffers, images, shaders, and pipelines until completion
  - can allocate private `VkShaderModule` objects from validated SPIR-V words behind opaque RHI
    handles
  - uploads RGBA8/sRGB 2D images, arrays, and mip chains through the shared persistently mapped
    staging ring and serial-tracked upload contexts into optimal-tiled images; queue ordering makes
    them visible to later draws without an immediate fence wait
  - can allocate private compute `VkPipeline` objects from owned compute shader modules and bound
    material pipeline layouts
  - creates private graphics pipelines with explicit topology, polygon/cull/front-face state,
    depth test/write/compare state, blend mode, vertex attributes, and target formats
  - can update private material descriptor sets for uniform scalar/color bindings from uploaded
    uniform buffers and sampled texture bindings from uploaded sampled images
  - polls frame/upload fences before descriptor mutation and rejects layout replacement, descriptor
    updates, or release of descriptor-referenced resources while the set is still in flight
  - exposes an owner-thread idle barrier used between the final application frame and mode
    teardown, so presentation resources can be released only after their last descriptor use
    completes; renderer shutdown repeats the barrier before destroying renderer-owned resources
  - executes the fixed unified sky, opaque terrain, alpha-tested terrain, rich/static,
    transparent/fluid, debug, UI, and present schema in one command buffer and one queue submission:
    acquire, draw into offscreen color/depth, blit color into the acquired swapchain image,
    transition it to present, and present once
  - pushes the view-projection matrix and camera-relative chunk origin before each terrain draw
  - accepts generic `execute_frame_plan` calls only for one external full-extent RGBA8-unorm clear
    target plus an optional present pass; `execute_frame` accepts only the exact two-resource,
    eight-pass unified schema and draw-command pass indices 1 through 6
  - returns `renderer.vulkan_unsupported_frame_plan` before submission when a valid renderer-neutral
    plan contains operations this backend cannot execute faithfully
  - translates the supported plans' RHI resource states into private Vulkan image layouts, access
    masks, and pipeline stages
  - records planned `VkImageMemoryBarrier`s for the swapchain target on present frames,
    making submitted barrier counts observable separately from the full frame-plan synchronization
    count without exposing Vulkan handles through the RHI
  - prefers `B8G8R8A8_SRGB` with the nonlinear sRGB color space for the swapchain and otherwise uses
    the first supported surface format; HDR, wide-gamut, and display color management are not
    implemented
  - can allocate private `VkDescriptorSetLayout`, `VkPipelineLayout`, `VkDescriptorPool`, and
    descriptor set objects for material pipeline layouts without exposing their handles
  - requests the Khronos validation layer when configured and enables `VK_EXT_debug_utils`
    independently when available, so pass/upload labels remain active in non-validation benchmark
    runs; warning and error callbacks route into engine logging, with graceful fallback when either
    facility is unavailable
  - records delayed timestamp-query results for the complete GPU frame, opaque terrain pass,
    frame-transfer interval, final swapchain copy, and asynchronous buffer-upload batches without
    stalling to read the current submission; upload timings retain their source submission serial
  - emits Vulkan debug labels around opaque terrain, frame transfer, final copy, and sampled-image
    upload command regions
  - reports unavailable when Vulkan is not compiled in or no graphics-capable physical device is
    present
  - does not expose Vulkan handles through the RHI

The renderer must stay below gameplay, modding, save, and simulation systems. Gameplay
code should not depend on Vulkan types, swapchain objects, descriptor handles, or
backend-specific allocation details. Future render features should enter through
engine-owned abstractions such as material definitions, asset handles, render passes,
debug draw, and validated shader-pack extension points.

The finalized gameplay boundary, ownership rules, frame lifecycle, coordinate conventions,
failure behavior, and performance budgets are specified in
[`renderer_v1_handoff.md`](renderer_v1_handoff.md).

The `apps/render_smoke` visible path now exercises the Milestone 2 front end. It opens a native X11
window, populates nine real far-world chunks, loads checked-in validated SPIR-V, and lets `Renderer`
budget meshing/uploads, retain GPU meshes, frustum-cull entries, and build the unified indexed world
pass. Camera-relative positioning, depth testing, camera controls, resize/minimize handling,
swapchain recreation, and clean shutdown all use `execute_frame()`; no separate
`bind_mesh_draws()` submission is required.

Milestone 3 instrumentation is exposed through `Renderer::stats()`. Scoped CPU zones distinguish
chunk synchronization, extraction, culling, draw-list and command construction, chunk snapshot
capture, meshing, upload preparation, upload copying, backend command recording, and time blocked
on acquire/fence/device synchronization. Vulkan timestamp results carry the source frame index and
latency in frames, so callers never confuse delayed GPU measurements with the current CPU frame.
The headless backend validates the same submission and counter structure while reporting GPU timing
as unavailable.

`apps/render_benchmark` provides deterministic flat, mountain, cave, checkerboard, cross-plane
forest, rapid-edit, high-speed flythrough, load/unload churn, large-coordinate, and
resize/minimize scenes. Integer/hash-based generation and frame-indexed stress schedules make the
workloads reproducible. The recorder excludes warm-up frames, retains complete per-frame renderer
statistics, computes median/p95/p99/max frame time and 1%/0.1% low FPS, and exports JSON or CSV.
Both exports carry a versioned schema and the complete run configuration (scene, seed, backend,
mesher, resolution, radius, warm-up/measurement counts, frame cap, and validation request); CSV
repeats summary statistics on each frame row so it remains self-contained for tabular analysis.
The summary aggregates every CPU and GPU interval independently and preserves the complete CPU
subsystem breakdown for the slowest frame, while the frame records retain delayed GPU source-frame
and upload-submission identities. A spike can therefore be attributed without aligning asynchronous
GPU results to the wrong CPU frame.
The timing definitions, workload catalog, and native/headless verification record are maintained in
`docs/performance/renderer_milestone_3.md`.
Rendering is uncapped unless a frame cap is explicitly requested. Headless is the automation
default; Vulkan mode opens a native window and adds GPU pass timings. Before warm-up and measured
simulation begin, the benchmark settles all initially loaded chunks to resident meshes. Streaming
and rapid-edit scenes therefore measure replacement/churn while retaining a valid baseline world,
rather than accidentally benchmarking an initial-mesh starvation loop.

Production asynchronous chunk meshing is owned by `ChunkRenderSystem`. The renderer-owner thread
copies a bounded center-plus-halo neighborhood and compact block render table before submitting a
job. Workers only receive immutable snapshots; they never query `WorldState`, `ChunkDatabase`,
`VoxelChunk`, or the prototype registry. A fixed worker pool publishes typed results through a
thread-safe mailbox. The owner thread rejects results from stale content, changed neighbors,
superseded load generations, or obsolete render tables both before upload preparation and again
before upload. Rebuilds keep the prior resident mesh visible, and rapid requests are coalesced or
cancelled before expensive work when possible.

The immutable block-render snapshot now pre-resolves full-cube versus specialized geometry,
material-table index, six-bit occlusion mask, render phase, emissive/two-sided/state flags, model
metadata, and neighbor radius before worker execution. `GreedyChunkMesher` is a separate optimized
full-cube path; the readable `ChunkMesher` remains the correctness reference and still handles
boxes, cross-plane foliage, and rich-model extraction. Greedy masks merge only faces with identical
material, voxel type, render phase, light, state, and relevant block flags. Directional surface-area
tests cover all six faces, negative chunk coordinates, cross-chunk occlusion, incompatible merge
keys, and checkerboard worst cases. In the deterministic flat nine-chunk benchmark this reduced the
resident terrain from 43,776 triangles to 108 without changing visible surface coverage. CPU meshes
also group their complete index range into validated, nonoverlapping
material/render-phase sections. The retained GPU cache preserves those ranges, and visibility
extraction emits one indexed draw per section while counting drawn chunks separately from draws.
Completed uploads return CPU vertex/index/section storage to a bounded scheduler pool and compact
GPU-vertex conversion storage to an owner-thread pool. Warm remeshes reuse those capacities; pool
counts and retained capacities are exposed in chunk renderer statistics.
The optimized cube loop scans each axis boundary once for both face directions, bounds work to the
occupied cell extent, and uses inline contiguous snapshot/render-table access. The benchmark runner
can select `--reference-mesher` for an otherwise identical baseline; recorded Debug and Release
results live in `docs/performance/renderer_milestone_8.md`.

The backend currently supports one Vulkan render sequence with six draw-producing logical phases
per unified submission, plus the narrower generic clear/present smoke schema. Arbitrary resource
graphs or additional Vulkan render passes, frame-local descriptor allocation for textured
materials, compressed texture/KTX2 handling, and a RenderDoc capture workflow belong to later
integration slices. Draw-command and visibility vectors retain their capacity between frames; the
remaining frame-plan metadata is small and will move into the broader frame allocator as more passes
are introduced.

Runtime shader programs use generation-safe `ShaderProgramHandle` values and retain stage entry
points, dependencies, and an explicit shader-interface contract. The shader manager validates all
SPIR-V before module creation. Development reload creates every replacement module before changing
the resident program, keeps superseded modules alive while dependent pipelines rebuild, and leaves
the previous program and terrain pipeline untouched on load failure. The renderer updates future
chunk draw packets only after a complete dependent-pipeline rebuild. `PipelineCache` keys graphics
pipelines by shader
program, vertex layout, render phase, attachment formats, raster/depth/blend state, sample count,
and feature flags. Common pipelines are prewarmed, then the cache is sealed so a normal frame cannot
create a surprise pipeline. Reload rebuilds dependent entries transactionally and preserves their
old pipelines if any replacement fails. Rebinding a byte-for-byte equivalent RHI layout is
idempotent so prewarming and shader replacement do not invalidate unrelated pipelines.

The application build has a real GLSL-to-SPIR-V path: when `glslangValidator`/`glslang` is
installed, CMake recompiles the terrain, static-mesh, debug-line, and UI vertex/fragment stages after
source changes and stages the build-tree artifacts for the renderer applications. Checked-in,
externally validated SPIR-V is the deterministic fallback when the tool is unavailable. Normal
runtime rendering never compiles shaders. The
general asset `ShaderCompiler` still has development validators plus a production SPIR-V
passthrough profile and reports `shader_compiler.production_compiler_unavailable` for Slang/HLSL
until that compiler backend is linked; shader-pack compilation remains a later extension of that
declared cook boundary.
