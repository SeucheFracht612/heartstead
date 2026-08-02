# Testing and verification

Heartstead uses CTest for unit, integration, regression, and headless-safe smoke coverage. Native
presentation checks and performance measurements complement the automated suite; they do not
replace it.

## Default suite

```bash
cmake --preset default-debug
cmake --build --preset default-debug
ctest --preset default-debug
```

Do not document a fixed number of tests. Enumerate the configured suite when needed:

```bash
ctest --preset default-debug --show-only
```

Useful CTest operations:

```bash
# Run tests whose names match a regular expression
ctest --preset default-debug -R 'network|replication'

# Run tests carrying the smoke label
ctest --preset default-debug -L smoke

# Repeat failures to expose flakes
ctest --preset default-debug --repeat until-fail:20

# Show output from failed tests
ctest --preset default-debug --output-on-failure
```

## Compiler and warning checks

Changes touching shared headers, templates, compiler-sensitive code, or public boundaries should
pass both maintained Linux compilers with warnings as errors:

```bash
cmake --preset linux-clang-debug-werror
cmake --build --preset linux-clang-debug-werror
ctest --preset linux-clang-debug-werror

cmake --preset linux-gcc-debug-werror
cmake --build --preset linux-gcc-debug-werror
ctest --preset linux-gcc-debug-werror
```

`default-debug-werror` is useful when only the host-default compiler is available.

## Sanitizers

AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan
ctest --preset linux-clang-asan
```

Run the same binaries with LeakSanitizer outside debugger/managed environments that interfere with
LeakSanitizer startup:

```bash
ctest --preset linux-clang-asan-leaks
```

ThreadSanitizer:

```bash
cmake --preset linux-clang-tsan
cmake --build --preset linux-clang-tsan
ctest --preset linux-clang-tsan
```

ASan/UBSan and TSan are intentionally separate builds. Sanitizer presets disable Vulkan so reports
focus on project-owned code.

## Profiling and performance gates

Use an optimized instrumented build for hierarchical Tracy captures:

```bash
cmake --preset profiling-release
cmake --build --preset profiling-release
ctest --preset profiling-release
```

Use a declared benchmark tier for an absolute automation gate. The runner writes its raw result
before returning status 2 on a failed evaluated gate:

```bash
./build/profiling-release/apps/render_benchmark/heartstead_render_benchmark \
  --headless --scene mountains --warmup 120 --frames 1000 --radius 2 \
  --budget minimum --output build/benchmarks/mountains-minimum.json
```

Headless runs cannot evaluate the GPU portion. Performance sign-off also requires repeated native
runs on the declared reference hardware; see [Renderer benchmarks](../performance/renderer_benchmarks.md).

## Runtime smoke checks

Exercise the complete local runtime without presentation:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game --headless --frames 120
```

Exercise native initialization and shutdown on a machine with X11 and Vulkan:

```bash
./build/default-debug/apps/dev_game/heartstead_dev_game \
  --native-frames 300 --no-save
```

For renderer-only native validation:

```bash
./build/default-debug/apps/render_smoke/heartstead_render_smoke
```

Exercise production asset loading and visual-prefab presentation without a display:

```bash
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --headless --prefab base:visuals/player --preview visual-prefab

ctest --preset default-debug -R '^smoke\.asset_lab$'
```

Install `VK_LAYER_KHRONOS_validation` for meaningful Vulkan validation. Absence of the optional
layer should be reported, not confused with a successful validation run.

## Renderer V2 checks

Build the warnings-as-errors configuration and run the focused renderer/environment suite:

```bash
cmake --preset default-debug-werror
cmake --build --preset default-debug-werror

ctest --test-dir build/default-debug-werror \
  -R 'renderer|render_scene|vegetation|water|environment_effects|particle|animation|equipment|entity_visual|visibility|hierarchical_depth|far_terrain|streaming_residency|ui_font|map_view|visual_regression|benchmark_coverage' \
  --output-on-failure
```

The focused groups cover:

| Area | Representative tests and manual workload |
| --- | --- |
| Environment and VFX | `heartstead_vegetation_renderer_tests`, `heartstead_large_water_renderer_tests`, `heartstead_environment_effects_tests`, particle tests, `starting-biome` |
| Characters and settlement state | animation/equipment/entity-visual tests, `character-workshop` |
| Large world and scalability | visibility, hierarchical-depth, streaming-residency, far-terrain and renderer-ownership tests; `flythrough`, `churn`, `large-coordinates`, `resize-minimize` |
| UI and hardening | UI/font/widget/map/quality/visual-regression/benchmark-coverage tests |

Exercise the deterministic integration scenes without a display:

```bash
mkdir -p build/renderer-checks
for scene in starting-biome character-workshop flythrough churn large-coordinates resize-minimize; do
  ./build/default-debug-werror/apps/render_benchmark/heartstead_render_benchmark \
    --headless --scene "${scene}" --warmup 5 --frames 30 --radius 1 \
    --output "build/renderer-checks/${scene}.json"
done
```

Exercise Vulkan, timestamps, debug labels, and validation on a present-capable machine. Validation
is requested by default; do not pass `--no-validation` for this check:

```bash
./build/default-debug-werror/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene starting-biome --warmup 5 --frames 30 --radius 1 \
  --width 960 --height 540 --output build/renderer-checks/starting-biome-vulkan.json

./build/default-debug-werror/apps/render_benchmark/heartstead_render_benchmark \
  --vulkan --scene character-workshop --warmup 5 --frames 30 --radius 1 \
  --width 960 --height 540 --output build/renderer-checks/character-workshop-vulkan.json
```

Inspect production assets and state mappings through the real presentation path:

```bash
./build/default-debug-werror/apps/asset_lab/heartstead_asset_lab \
  --headless --prefab base:visuals/player --preview character

./build/default-debug-werror/apps/asset_lab/heartstead_asset_lab \
  --headless --prefab base:visuals/workshop_machine --preview visual-prefab \
  --state activity=active --state process=finished --state heat=hot

./build/default-debug-werror/apps/asset_lab/heartstead_asset_lab \
  --headless --prefab base:visuals/stateful_crate --preview visual-prefab \
  --state fill=full --state access=open --state carry=carried
```

For interactive sign-off, run `heartstead_dev_game --native-frames 1200 --no-save`. Check the
always-visible minimap, press `M` for the full map and Escape to close it, resize/minimize the
window, edit chunk boundaries rapidly, visit water, and inspect `F3` diagnostics. High is the game
default. The player Options screen exposes all four tested policy presets, although not every
environment subsystem consumes every quality value yet.

Use [Visual regression](visual_regression.md) for PNG capture/compare and
[Renderer benchmarks](../performance/renderer_benchmarks.md) for performance methodology.

## Multiplayer checks

Use two processes for a real socket path rather than relying only on the in-memory transport:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server \
  --bind 127.0.0.1:7777

./build/default-debug/apps/heartstead/heartstead \
  --connect 127.0.0.1:7777
```

Validate connection, command results, movement prediction/reconciliation, world updates,
disconnect, timeout, and clean shutdown. The dedicated executable is memory-only, so persistence
must be tested through a local authoritative runtime until dedicated save ownership is implemented.

For repeatable eight-client chunk-interest, hot-edit, and conditioned queue/private-memory soak
acceptance, run the optimized
[`heartstead_multiplayer_chunk_subscription_benchmark`](../performance/multiplayer_chunk_subscription_benchmarks.md)
with `--enforce-gates --require-precise-memory`. The second option deliberately fails calibration
when the host cannot provide precise process-memory accounting; omit it only for a functional run
whose memory gates are not acceptance evidence.

For repeatable impaired-runtime acceptance, run the optimized
[`heartstead_multiplayer_network_impairment_benchmark`](../performance/multiplayer_network_impairment_benchmarks.md)
with `--enforce-gates`. For the real socket path, use `tools/netem_multiplayer.sh` on Linux to apply
a documented latency/delay-variation/loss profile. Do not turn one observed packet count or
correction distance into a permanent architecture claim; keep durable acceptance thresholds in
tests and record individual runs as artifacts.

## Game-shell lifecycle checks

Run the state, front-end, session, and repeated-replacement coverage with:

```bash
ctest --test-dir build/default-debug-werror \
  -R 'application_state|front_end|runtime_session_lifecycle|game_shell_lifecycle|heartstead_.*shutdown_smoke' \
  --output-on-failure
```

`heartstead_game_shell_lifecycle_stress_tests` warms the allocator, then replaces generated and
packaged far-coordinate sessions repeatedly, reloads a persistent save, and checks teardown
counters after each transition. On Linux it also bounds RSS growth and checks that thread and open
file counts do not accumulate. Pair it with leak detection for ownership changes:

```bash
cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan --target heartstead_game_shell_lifecycle_stress_tests
ctest --preset linux-clang-asan-leaks -R game_shell_lifecycle_stress
```

The F3 panel is the native counterpart: record renderer object counts and Vulkan device memory
before and after several world switches. A headless pass is not evidence of GPU-memory stability.

## Benchmarks

Use optimized builds for performance decisions:

```bash
cmake --preset default-release
cmake --build --preset default-release \
  --target heartstead_render_benchmark heartstead_audio_benchmark \
           heartstead_ui_benchmark heartstead_scripting_benchmark
```

Benchmarks are not pass/fail substitutes for functional tests. Record the commit, machine, build,
backend, arguments, output file, and environmental conditions for every published comparison. See
[Renderer benchmarks](../performance/renderer_benchmarks.md) for renderer-specific methodology.

## Change-completion checklist

Before considering a change complete:

1. Run the most focused affected tests while iterating.
2. Build and run `ctest --preset default-debug`.
3. Use a warning-as-error preset for shared or public code.
4. Run the sanitizer relevant to changed ownership, parsing, threading, or memory behavior.
5. Exercise a native path when changing windows, input, Vulkan, audio, or presentation.
6. Exercise the socket-backed path when changing networking or replication.
7. Add regression coverage for every corrected defect.
8. Update the maintained documentation and executable examples in the same change.
9. Confirm no new unbounded queue, allocation, payload, retry, or file scan was introduced.
10. Keep exact historical measurements in benchmark artifacts, not architecture prose.
