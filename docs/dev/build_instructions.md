# Build Instructions

Heartstead uses vcpkg manifest mode for native dependencies. Install and bootstrap vcpkg, then
export its root before configuring:

```bash
export VCPKG_ROOT=/absolute/path/to/vcpkg
```

The checked-in manifest pins the registry baseline and dependency versions. CMake restores the
manifest into the selected build tree through the vcpkg toolchain.

Configure:

```bash
cmake --preset default-debug
```

Build:

```bash
cmake --build --preset default-debug
```

Test:

```bash
ctest --preset default-debug
```

The default CTest preset runs the unit tests and headless-safe smoke tests for the default-built
samples and tools. The commands below are still useful when you want to run one boundary manually.

Strict warning build:

```bash
cmake --preset default-debug-werror
cmake --build --preset default-debug-werror
ctest --preset default-debug-werror
```

Pin the strict build to each supported Linux compiler when checking portability-sensitive changes:

```bash
cmake --preset linux-clang-debug-werror
cmake --build --preset linux-clang-debug-werror
ctest --preset linux-clang-debug-werror

cmake --preset linux-gcc-debug-werror
cmake --build --preset linux-gcc-debug-werror
ctest --preset linux-gcc-debug-werror
```

AddressSanitizer plus UndefinedBehaviorSanitizer build:

```bash
cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan
ctest --preset linux-clang-asan
ctest --preset linux-clang-asan-leaks
```

ThreadSanitizer build:

```bash
cmake --preset linux-clang-tsan
cmake --build --preset linux-clang-tsan
ctest --preset linux-clang-tsan
```

The sanitizer presets use Clang, enable warnings-as-errors, and disable Vulkan so the checks focus
on Heartstead-owned foundation code instead of graphics driver behavior. Use `linux-clang-asan` for memory and
undefined-behavior checks. Use `linux-clang-tsan` separately for data-race checks; it cannot be
combined with AddressSanitizer. The default ASan CTest preset disables LeakSanitizer detection
because managed/debugger-style environments can run tests under `ptrace`, where LeakSanitizer
aborts at startup. Run the `linux-clang-asan-leaks` preset outside such an environment to execute
the same complete suite with leak detection enabled.

Run the development asset cooker:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker
./build/default-debug/tools/shader_compiler/heartstead_shader_compiler
```

Print cooked asset store inspection data while cooking, or inspect an existing cooked output:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker . build/cooked_assets/asset_manifest.txt development --inspect
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker --inspect-store build/cooked_assets
```

Cook one active logical asset with the production converter, which is useful for staging a focused
runtime or tool fixture without requiring unrelated partial production converters:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker . build/cooked_model/asset_manifest.txt production --only base:models/entities/storybook_player.gltf
```

Print shader compile inspection data:

```bash
./build/default-debug/tools/shader_compiler/heartstead_shader_compiler . build/compiled_shaders/shader_manifest.txt development --inspect
```

Run the production shader profile for validated SPIR-V passthrough:

```bash
./build/default-debug/tools/shader_compiler/heartstead_shader_compiler . build/compiled_shaders/production_shader_manifest.txt production
```

Run smoke samples:

```bash
./build/default-debug/samples/platform_smoke/heartstead_platform_smoke
./build/default-debug/samples/renderer_smoke/heartstead_renderer_smoke
./build/default-debug/samples/physics_sandbox/heartstead_physics_sandbox
./build/default-debug/samples/network_sandbox/heartstead_network_sandbox
./build/default-debug/samples/scripting_sandbox/heartstead_scripting_sandbox
./build/default-debug/samples/jobs_sandbox/heartstead_jobs_sandbox
./build/default-debug/samples/math_sandbox/heartstead_math_sandbox
./build/default-debug/samples/world_state_sandbox/heartstead_world_state_sandbox
```

Run the retained Milestone 2 terrain renderer:

```bash
./build/default-debug/apps/render_smoke/heartstead_render_smoke
```

This application requires an X11 display and a Vulkan device that can present to it. It creates a
known nine-chunk far-world terrain set and exercises the retained `Renderer`, budgeted asynchronous
meshing, staged upload queues, GPU chunk cache, frustum culling, and unified indexed-draw frame
path. Use
WASD to move, Space to rise, hold the right mouse button to look, and press Escape or close the
window to exit. Resizing and minimizing the window preserve resident chunk buffers. If
`VK_LAYER_KHRONOS_validation` is installed, it is enabled automatically; otherwise startup
continues with a visible warning.

The shared, checked-in shader sources and production SPIR-V are under
`engine/renderer/shaders/builtin`. CMake detects `glslangValidator` (or the newer `glslang` binary)
and, when available, recompiles
the GLSL into the build tree whenever a source changes. Otherwise it stages the checked-in SPIR-V,
so release builds never require a runtime shader compiler. To regenerate and validate the
checked-in artifacts with external Khronos tools:

```bash
for shader in \
  sky.vert sky.frag \
  terrain.vert terrain.frag \
  static_mesh.vert static_mesh.frag \
  debug_line.vert debug_line.frag \
  ui.vert ui.frag; do
  glslangValidator -V --target-env vulkan1.0 \
    "engine/renderer/shaders/builtin/${shader}" \
    -o "engine/renderer/shaders/builtin/${shader}.spv"
  spirv-val "engine/renderer/shaders/builtin/${shader}.spv"
done
```

Both renderer applications stage the selected artifacts beside their executable. The runtime loads
and validates the SPIR-V header before creating Vulkan shader modules; it never compiles shader
source during normal rendering.

Run a deterministic renderer benchmark headlessly:

```bash
./build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --scene mountains --warmup 120 --frames 1000 --radius 2 \
  --output build/benchmarks/mountains.json
```

Use the `default-release` preset for optimized renderer measurements. Add `--reference-mesher` to
run the readable correctness mesher through the same benchmark pipeline as an optimization
baseline. Milestone 8 reference results and machine details are recorded in
`docs/performance/renderer_milestone_8.md`.

Use the native Vulkan backend to collect delayed GPU timestamp measurements:

```bash
./build/default-debug/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene checkerboard --warmup 120 --frames 1000 --radius 1 \
  --format csv --output build/benchmarks/checkerboard-vulkan.csv
```

The runner is uncapped by default. Add `--frame-cap 60` only when a capped comparison is intended.
Use `--list-scenes` for all deterministic workloads. JSON contains a summary plus full per-frame
records; CSV contains the same timing and renderer-counter columns and repeats the run summary on
each row for standalone analysis. Both formats record a versioned schema plus backend, mesher,
initial resolution, chunk radius, warm-up/measured frame counts, frame cap, validation request,
scene, and seed so a result file carries the configuration needed to reproduce it. The summary
reports median, 95th/99th percentiles, 1%/0.1% low FPS, maximum frame time, and mean CPU, GPU,
meshing, upload, and GPU-wait times. It also reports mean extraction, synchronization, culling,
draw-list, command-build, command-recording, snapshot, upload-preparation, and GPU pass intervals,
plus the full CPU timing breakdown of the slowest measured frame. Vulkan validation is requested by
default and remains optional when the Khronos layer is not installed.
The measurement semantics and latest verification matrix are recorded in
`docs/performance/renderer_milestone_3.md`.

Run mod/prototype validation tools:

```bash
./build/default-debug/tools/mod_validator/heartstead_mod_validator
./build/default-debug/tools/mod_validator/heartstead_mod_validator . --inspect
./build/default-debug/tools/prototype_inspector/heartstead_prototype_inspector
./build/default-debug/tools/prototype_inspector/heartstead_prototype_inspector base:items/raw_clay
./build/default-debug/tools/replay_inspector/heartstead_replay_inspector tests/fixtures/sample_command_replay.txt
./build/default-debug/tools/save_inspector/heartstead_save_inspector tests/fixtures/minimal_save_snapshot.txt
./build/default-debug/tools/save_inspector/heartstead_save_inspector --slots tests/fixtures/save_slots
./build/default-debug/tools/shader_compiler/heartstead_shader_compiler
./build/default-debug/tools/workpiece_inspector/heartstead_workpiece_inspector tests/fixtures/sample_workpiece_grid.txt
./build/default-debug/tools/world_inspector/heartstead_world_inspector tests/fixtures/minimal_save_snapshot.txt .
```

The `linux-clang-debug` and `linux-gcc-debug` presets pin specific compilers for CLion
profiles. Use `default-debug` when the host default compiler is preferred.
