# Architecture

## Source modules

- `core` owns shared math, primitive types and parallel job helpers.
- `voxel` owns block definitions, adaptive chunk storage and greedy meshing.
- `world` owns chunk collections, deterministic generation and streaming batches.
- `game` separates camera control, player movement, and block interaction without GLFW or OpenGL dependencies.
- `platform` owns operating-system and GLFW window services.
- `render` owns OpenGL resources, visibility and renderer UI presentation.
- `app` owns the small coordination services that intentionally bridge input, gameplay, world data, and rendering.

The executable entry point owns the frame loop while `platform::InputRouter`, `app::MenuController`, `app::VideoSettingsController`, and `app::WorldEditor` keep input callbacks, screen navigation, menu interaction, and block remeshing out of `main.cpp`. CMake exposes separate `Heartstead::Engine` and `Heartstead::Gameplay` targets.

The front end is state-driven. Main Menu, the saved-world browser, Singleplayer creation, Multiplayer, the two-action pause menu, Video Settings, and active gameplay share the same renderer but have separate input rules. Menu screens render over a slowly rotating camera in the generated voxel scene, so the panorama always represents the current terrain renderer rather than a bundled screenshot.

`app::WorldSaveStore` persists a versioned text format under `saves/`. Terrain is regenerated deterministically; only metadata, player/camera state, and the sparse `WorldEdits` overlay are serialized. Writes go to a temporary file and retain the previous file as a short-lived backup during replacement. The application saves at creation, every ten seconds, when returning to the main menu, and during normal shutdown.

## Why C++23

The target is a native Windows/Linux engine with explicit control over memory layout, allocation, threading, SIMD, and Vulkan. C++ provides that control, mature compiler tooling on both platforms, and direct access to graphics and profiling APIs. C++ is not automatically fast: the design, data access patterns, algorithms, and measurement discipline determine performance.

## Chunk dimensions and layout

Chunks contain 32 cubed voxels. This is a useful starting balance:

- coordinates fit compactly in mesh vertices;
- one direct chunk is 64 KiB of block IDs;
- jobs are large enough to amortize scheduling overhead;
- visibility and streaming remain granular.

The linear index is `x + 32 * (z + 32 * y)`. X is contiguous because terrain generation and greedy-meshing rows commonly advance along X. This decision must be re-profiled when lighting and simulation arrive.

## Adaptive storage

`Chunk` has three storage states:

- **Uniform:** one block ID for the entire chunk.
- **Palette:** a local block palette plus packed indices using 1-8 bits per voxel.
- **Direct:** one 16-bit block ID per voxel after the palette exceeds 256 entries.

Palette repacking is intentionally paid when a new bit width is crossed, not on reads. Terrain usually has a small palette and editing introduces new values rarely compared with reads and mesh passes.

## Meshing contract

The greedy mesher consumes a chunk and an immutable block registry and returns plain vertex/index arrays. It owns no renderer state. A render backend can upload these arrays directly or perform a later packing/transcoding step.

Each mesh job receives six optional read-only neighbor views. Boundary faces are emitted only by the chunk that owns the rendered voxel, while opaque faces between loaded chunks are culled. Missing neighbors are treated as air. Remeshing must be scheduled for both chunks when a neighbor loads, unloads, or changes along a shared edge.

At long range, dark vertical faces between one-block height terraces become smaller than one pixel and can alias into radial point patterns. The bootstrap renderer removes procedural high-frequency shading, uses 4x MSAA, fades directional-light contrast from 64 to 320 blocks, and applies atmospheric fog beyond 900 blocks. This preserves nearby block definition while stabilizing distant terrain.

True single-pixel T-junction leaks are covered by a second opaque-terrain draw placed 0.020 blocks inside the solid and padded only 0.004 blocks along each quad's tangent edges. Transparent leaves are excluded. The normal mesh is then drawn at its exact position and wins the depth test, so every visible block remains exactly 1 by 1 by 1 and the underlay is exposed only through a genuine rasterization crack. This adds no mesh memory and avoids coplanar z-fighting.

The underlay pass is discarded within 128 blocks of the camera. Nearby grass, stone, and other opaque blocks therefore use only their exact cube geometry, preventing the crack helper from producing visible outlines at close range.

## Video settings

Escape toggles a renderer-owned pixel-art Video Settings overlay. Render Distance is the diameter of the circular full-voxel chunk set. Its default is 64 chunks; double-clicking the number opens a numeric editor accepting 4-128 chunks. Applying a changed distance queues a complete deterministic voxel world around the camera. Distance Smoothing controls the starting distance of texture/normal stabilization from 128 through 640 blocks and previews immediately while the menu is open.

## Test world

The windowed demo keeps a circular 64-chunk-diameter full-voxel world (3,228 visible chunks plus a one-chunk generation border) at its default settings. Every visible chunk uses adaptive voxel storage, receives normal terrain and tree generation, and is processed by the neighbor-aware greedy mesher. The generated border prevents the visible mesh from emitting artificial vertical walls at its edge. A circular set removes 21.5 percent of the unused corners of the former square while preserving the same distance in every direction.

Mesh positions are local to their scene center while generation and terrain sampling remain in world coordinates. This keeps compact 16-bit vertices valid as the player travels. The desired scene center is placed eight chunks along the horizontal view direction. Crossing six chunks or turning far enough queues a replacement scene on a background task; the renderer continues drawing the previous scene until CPU generation and meshing finish, then uploads and swaps the new scene on the render thread.

Trees are selected from jittered world-space cells, then filtered by a seeded forest-density field, local slope, and elevation. This gives deterministic spacing without a regular grid appearance. Canopies may cross horizontal chunk boundaries because placement uses global block coordinates.

Chunk vertices share one exactly-sized GPU upload, while opaque and cutout faces use separate index buffers. Each chunk retains its index ranges and local bounds. Every frame, six camera-frustum planes reject invisible chunk bounds, the survivors are sorted front-to-back, and their ranges are submitted with `glMultiDrawElements`. The seam underlay and normal terrain pass process visible opaque ranges only; leaf/glass cutouts are submitted once afterward with alpha-to-coverage. The window title reports visible and total chunk counts for profiling.

## Block materials

The bootstrap renderer synthesizes deterministic 8 by 8 pixel-art patterns from face UV coordinates. Grass, dirt, stone, logs, and leaves each have a small procedural palette. Texture variation fades with distance to avoid moire aliasing. Leaf pixels combine deterministic cutouts with 70 percent alpha and alpha-to-coverage, giving partial transparency without a sorted transparent draw pass.

## Threading model (next milestone)

The intended pipeline is:

```text
requested coordinates -> generation queue -> immutable chunk snapshot
                    snapshot -> mesh queue -> revision-checked upload queue
                       render thread -> batched GPU upload -> visible draw list
```

The main rule is that worker jobs never mutate renderer-owned resources. Results cross queues as owned buffers tagged with chunk coordinates and revision numbers. A stale result is discarded cheaply.

## Performance targets

Initial targets for a mid-range desktop in a release build:

- frame time below 8.3 ms at 120 Hz;
- main/render thread submission below 2 ms;
- no allocation per voxel and no allocation per face;
- chunk mesh jobs below 1 ms for ordinary terrain, measured as a distribution;
- bounded streaming memory configured by view distance.

Targets are hypotheses until recorded on actual target hardware. The included benchmark establishes the first local baseline.
