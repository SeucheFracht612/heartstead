# Build instructions

Heartstead is a C++23 CMake project. It uses checked-in CMake presets, Ninja, and vcpkg manifest
mode so dependency versions and common build configurations stay reproducible.

## Requirements

- CMake 3.25 or newer;
- Ninja;
- a C++23-capable compiler (the maintained Linux presets use GCC or Clang);
- Git and a bootstrapped vcpkg checkout;
- development packages needed by the selected optional backends.

For the complete native Linux presentation path, install a Vulkan loader/SDK and X11/XRandR
development packages. CMake can build headless configurations without Vulkan. Jolt, miniaudio,
Luau, and other dependencies are restored through the repository's vcpkg manifest when enabled.

Point CMake at vcpkg before configuring:

```bash
export VCPKG_ROOT=/absolute/path/to/vcpkg
```

The manifest and vcpkg baseline are the dependency-version source of truth.

## Configure, build, and test

```bash
cmake --preset default-debug
cmake --build --preset default-debug
ctest --preset default-debug
```

Build one target while iterating:

```bash
cmake --build --preset default-debug --target heartstead
```

List targets known to the configured build:

```bash
cmake --build --preset default-debug --target help
```

## Presets

| Configure/build preset | Purpose |
| --- | --- |
| `default-debug` | Host-default compiler, debug build, normal development features. |
| `default-debug-werror` | Host-default compiler with warnings treated as errors. |
| `default-release` | Optimized build for benchmarks and release-like checks. |
| `linux-clang-debug` | Explicit Clang debug configuration. |
| `linux-clang-debug-werror` | Explicit Clang with warnings as errors. |
| `linux-gcc-debug` | Explicit GCC debug configuration. |
| `linux-gcc-debug-werror` | Explicit GCC with warnings as errors. |
| `linux-clang-asan` | Clang AddressSanitizer + UndefinedBehaviorSanitizer build. |
| `linux-clang-tsan` | Clang ThreadSanitizer build. |

CTest also defines `linux-clang-asan-leaks`, which runs the ASan-built suite with leak detection
enabled. The default ASan test preset disables LeakSanitizer because debugger/managed environments
that use `ptrace` can make LeakSanitizer abort before tests start.

Examples:

```bash
cmake --preset linux-clang-debug-werror
cmake --build --preset linux-clang-debug-werror
ctest --preset linux-clang-debug-werror

cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan
ctest --preset linux-clang-asan
ctest --preset linux-clang-asan-leaks

cmake --preset linux-clang-tsan
cmake --build --preset linux-clang-tsan
ctest --preset linux-clang-tsan
```

ASan/UBSan and TSan are separate configurations and must not be combined. The sanitizer presets
turn Vulkan off so they focus on Heartstead-owned code rather than graphics-driver internals.

## CMake options

The top-level project builds applications, samples, tools, and tests by default. Optional engine
backends are discovered or enabled through CMake options, including Vulkan, X11/XRandR, Jolt,
miniaudio, and Luau. Inspect the cache of an already configured preset rather than copying an old option list:

```bash
cmake -N -LAH build/default-debug
```

For a focused headless configure, pass the relevant `HEARTSTEAD_ENABLE_*` options explicitly or use
one of the sanitizer presets as a reference. Do not assume that a backend being disabled means its
higher-level contract disappears; headless and disabled backends intentionally keep many tools and
tests functional.

## Shaders

Built-in shader source and checked-in validated SPIR-V live under
`engine/renderer/shaders/builtin`. When `glslangValidator` or `glslang` is available, CMake rebuilds
SPIR-V in the build tree after source changes. Otherwise it stages the checked-in payloads. Normal
runtime rendering does not compile shader source.

To regenerate every checked-in built-in shader with Khronos tools:

```bash
find engine/renderer/shaders/builtin -maxdepth 1 -type f \
  \( -name '*.vert' -o -name '*.frag' -o -name '*.comp' \) -print0 |
while IFS= read -r -d '' source; do
  shader="${source##*/}"
  glslangValidator -V --target-env vulkan1.0 \
    "${source}" \
    -o "engine/renderer/shaders/builtin/${shader}.spv"
  spirv-val "engine/renderer/shaders/builtin/${shader}.spv"
done
```

The general asset shader compiler is a separate cook/validation boundary. Its production profile
accepts validated SPIR-V passthrough; production Slang/HLSL compilation requires a linked compiler
backend.

## Asset and validation tools

Examples after a debug build:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker
./build/default-debug/tools/shader_compiler/heartstead_shader_compiler
./build/default-debug/tools/mod_validator/heartstead_mod_validator . --inspect
./build/default-debug/tools/prototype_inspector/heartstead_prototype_inspector
```

Cook one active asset without requiring unrelated production converters:

```bash
./build/default-debug/tools/asset_cooker/heartstead_asset_cooker \
  . build/cooked_model/asset_manifest.txt production \
  --only base:models/entities/storybook_player.gltf
```

Build and exercise the production presentation inspector without a display:

```bash
cmake --build --preset default-debug --target heartstead_asset_lab
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --headless --prefab base:visuals/player --preview visual-prefab
```

See [the asset pipeline guide](../asset_pipeline.md) for source layout, formats, licenses, cooking,
and runtime lookup. See [Asset Lab](../asset_lab.md) for preview modes and inspection controls.

## Troubleshooting

- **Preset cannot find vcpkg:** verify `VCPKG_ROOT` points to a bootstrapped checkout before the
  first configure, then remove the failed build directory and configure again.
- **Vulkan application is unavailable:** confirm the Vulkan loader, headers, and a present-capable
  driver are installed and that CMake found Vulkan.
- **Native window path is unavailable:** the current maintained path requires X11 and XRandR.
- **Validation layer warning:** `VK_LAYER_KHRONOS_validation` is optional at runtime. Install it for
  graphics validation; the application reports when it is not present.
- **Stale generated files:** use a fresh preset build directory rather than mixing generators,
  compilers, or vcpkg roots in one tree.
- **A command in documentation differs from the executable:** check the executable's `--help`; it
  is the command-line source of truth.

Continue with [Running Heartstead](running.md) or [Testing](testing.md).
