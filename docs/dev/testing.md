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

## Multiplayer checks

Use two processes for a real socket path rather than relying only on the in-memory transport:

```bash
./build/default-debug/apps/dedicated_server/heartstead_dedicated_server \
  --bind 127.0.0.1:7777

./build/default-debug/apps/dev_game/heartstead_dev_game \
  --connect 127.0.0.1:7777
```

Validate connection, command results, movement prediction/reconciliation, world updates,
disconnect, timeout, and clean shutdown. The dedicated executable is memory-only, so persistence
must be tested through a local authoritative runtime until dedicated save ownership is implemented.

For impaired-network testing, use `tools/netem_multiplayer.sh` on Linux to apply a documented
latency/jitter/loss profile. Do not turn one observed packet count or correction distance into a
permanent architecture claim; keep durable acceptance thresholds in tests and record individual
runs as artifacts.

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
