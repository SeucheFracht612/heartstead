# Heartstead Voxel Engine

A from-scratch, performance-first voxel engine for Windows and Linux. The project uses C++23 and CMake, with a core that does not depend on a game engine or third-party runtime.

This is milestone 0.2: a windowed, GPU-rendered voxel demo plus the data and geometry pipeline on which streaming, physics, and gameplay will be built.

## What exists now

- 32 x 32 x 32 chunks with X-major memory order for contiguous row access.
- Adaptive voxel storage: uniform, bit-packed palette, then direct 16-bit blocks.
- Revision tracking so jobs can discard stale mesh results without locks.
- Greedy meshing across all six face directions.
- Compact 16-byte, integer-positioned render vertices.
- OpenGL 3.3 GPU rendering with depth testing, back-face culling, and directional lighting.
- Minecraft-scale player controller with walking, jumping, sprinting, noclip flight, and first/third-person cameras.
- Live rotating terrain panorama main menu with Singleplayer, Multiplayer, and Video Settings navigation.
- Saved-world browser plus a separate world-creation screen with an editable name, local creation date, mode, terrain type, seed, and view-distance information.
- Compact procedural save files containing metadata, player/camera position, and voxel edits, with timed autosaves and save-on-exit.
- A circular 64-chunk-diameter full-voxel world (3,228 visible chunks) with normal block trees throughout.
- Hierarchical 4 x 4 region frustum/occlusion culling, cached front-to-back ordering, and batched OpenGL multi-draw.
- Separate opaque and cutout index streams so transparent leaves skip the terrain crack-underlay pass.
- Asynchronous position- and view-aware recentering that keeps the current scene visible while the next one is generated.
- Parallel terrain generation and chunk meshing across the available CPU threads.
- Neighbor-aware chunk meshing that removes hidden faces at chunk seams.
- Seeded, non-periodic terrain with rolling hills and ridged mountain regions.
- Regional lowlands, sandy basins, exposed stone ranges, and snow-capped high peaks.
- Deterministically placed oak trees with trunks and partially transparent leaf canopies.
- Procedural 8 x 8 pixel-art materials for grass, dirt, stone, bark, and leaves.
- Cross-platform CMake targets for the demo, tests, and benchmark.
- No external dependencies in the engine core.

## Build

Install a C++23 compiler, Git, and CMake 3.24 or newer.

After the first build, launch the game with `launch-game.cmd` on Windows or `./launch-game.sh` on Linux. The launchers automatically build the game if its executable is missing.

### Windows (Visual Studio 2022)

```powershell
./scripts/build-windows.cmd
./build-windows-ninja/heartstead.exe
./build-windows-ninja/heartstead_bench.exe
```

The script locates Visual Studio Build Tools, initializes its x64 compiler environment, configures a release build, and runs the tests.

The first configure downloads the pinned GLFW 3.4 source used for native window and input handling. Run `heartstead.exe` to open the main menu over a live terrain panorama. Singleplayer opens the saved-world browser, where an existing world can be played or a new one created. Multiplayer opens the server-browser screen, and Settings opens Video Settings. In game: **W/A/S/D** move, **mouse** looks, **Space** jumps, **Left Shift** sprints, **left/right mouse** breaks/places blocks, **F** toggles flight, **F3** toggles performance diagnostics, **F5** changes camera view, and **Escape** opens a pause panel containing Settings and Main Menu. Press Escape again to resume. In flight mode, **Space** rises and **Left Ctrl** descends.

Worlds are stored in the project-local `saves` directory. Creating a world writes it immediately; active worlds autosave every ten seconds and are saved again when returning to the main menu or closing the game. Generated terrain is deterministic, so save files store only metadata, player/camera state, and changed blocks.

The Video Settings panel defaults to a circular full-voxel world with a 64-chunk diameter. Double-click the Render Distance value to enter a custom 4-128 chunk diameter. **Distance Smoothing** controls distant texture and lighting stabilization. **Shadow Distance** limits the expensive shadow-map region and fades shadows smoothly near its edge. **VSync** and **Fullscreen** apply when the panel closes. Distance changes queue a new deterministic full-voxel scene without removing the currently rendered scene.

### Linux (Clang or GCC)

The source is cross-platform, but no native program can be guaranteed on every Linux installation. Heartstead requires a 64-bit C++23 compiler, CMake 3.24+, an OpenGL 3.3-capable driver, and GLFW's Linux development dependencies. On Debian/Ubuntu, `xorg-dev`, `libgl1-mesa-dev`, `cmake`, and `g++` provide the usual X11 build path. Wayland desktops can run this build through XWayland.

```bash
./scripts/build-linux.sh
./build-linux/heartstead
./build-linux/heartstead_bench
```

After the first build, `./launch-game.sh` incrementally rebuilds and launches the game, matching the Windows launcher workflow. If the downloaded scripts do not retain executable permissions, run them with `bash launch-game.sh` and `bash scripts/build-linux.sh`.

For machine-specific release builds, add `-DHEARTSTEAD_ENABLE_NATIVE_ARCH=ON`. Do not use that option for binaries distributed to other machines.

## Architecture direction

The engine is deliberately split into stages:

1. **World data:** sparse chunk map, adaptive block storage, immutable read snapshots.
2. **Generation:** deterministic, seed-based jobs producing chunk data only.
3. **Meshing:** parallel CPU jobs and neighbor-aware greedy faces.
4. **Rendering:** current OpenGL 3.3 bootstrap backend, followed by Vulkan and GPU-driven culling/indirect draws.
5. **Simulation:** fixed-timestep ECS, broad-phase spatial indexing, and separately budgeted lighting/fluid updates.

The next milestone should add chunk-neighbor sampling, a sparse multi-chunk world, asynchronous generation/mesh upload, and the Vulkan backend. Graphics API code remains behind a narrow renderer boundary so the voxel pipeline stays independently testable.

## Performance rules

- Profile release builds; debug timings are meaningless.
- Keep chunk generation and meshing off the render thread.
- Batch allocation and GPU uploads rather than allocating per voxel or per quad.
- Use handles and revisions across threads, not owning pointers into mutable world state.
- Add SIMD only after measurements identify a stable hot loop.
- Treat every visual feature as a budget in milliseconds and bytes.
