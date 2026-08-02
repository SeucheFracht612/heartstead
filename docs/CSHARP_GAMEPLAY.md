# C# gameplay layer

Heartstead can keep its performance-critical engine in C++ while running game rules in C#. This is not exactly Unreal's model—Unreal exposes C++ through reflection and Blueprints—but it creates the same useful separation between a native engine and rapidly editable gameplay code.

## Ownership boundary

Keep these systems native C++:

- voxel and chunk storage;
- world generation, meshing, streaming and job scheduling;
- renderer, GPU resources, visibility, lighting and shadows;
- collision queries and broad-phase spatial data;
- native window, input collection and audio backends.

Good C# systems include:

- player rules and abilities;
- inventory, crafting, items and status effects;
- creatures, AI state machines and quests;
- interaction rules and block-selection policy;
- menus and higher-level UI state;
- mod-facing game APIs.

Do not cross the native/managed boundary once per voxel, face or draw call. C++ should expose batched queries and command buffers so one managed gameplay tick needs only a few calls.

## Proposed runtime flow

```text
GLFW input + native world snapshot
              |
              v
       C# Gameplay.Tick
              |
              v
 movement / edit / spawn command buffer
              |
              v
 C++ validation, physics, world mutation and rendering
```

The first managed version should replace the policy inside `game/player`, not native collision or voxel storage. For example, C# decides desired movement, jumping and interactions; C++ validates and executes those commands against the world.

## Native interface rules

Use a versioned C ABI instead of exporting C++ classes:

```cpp
struct HsGameplayApiV1 {
    std::uint32_t version;
    bool (*raycast)(HsRay ray, HsRayHit* hit);
    bool (*queue_block_edit)(HsBlockEdit edit);
    void (*read_player)(HsPlayerHandle player, HsPlayerState* state);
};
```

- Pass fixed-width integers, floats, blittable structs and opaque integer handles.
- Never pass STL containers, C++ exceptions, owning pointers or renderer objects.
- Version the function table so old gameplay assemblies fail cleanly.
- Catch managed exceptions at the managed entry point; never unwind across the ABI.
- Batch variable data into caller-owned spans or command buffers.
- Keep managed per-frame allocations near zero to avoid garbage-collection spikes.

## Hosting plan

1. Add a small `scripting/dotnet_host` C++ module that locates `hostfxr` through `nethost`.
2. Load `Heartstead.Gameplay.runtimeconfig.json` and obtain `load_assembly_and_get_function_pointer`.
3. Load one static managed bootstrap entry point from `Heartstead.Gameplay.dll`.
4. Pass `HsGameplayApiV1` to managed initialization and retain managed `Initialize`, `Tick`, and `Shutdown` entry points.
5. Initially disable scripting cleanly when .NET is unavailable, allowing the native engine and tests to run alone.
6. Add managed reload through a collectible `AssemblyLoadContext` only after the basic lifetime is reliable.

Microsoft's supported native-hosting flow uses `hostfxr` and `load_assembly_and_get_function_pointer`: https://learn.microsoft.com/dotnet/core/tutorials/netcore-hosting

Managed functions callable directly from native code can use `UnmanagedCallersOnly`; such methods must be static and use only blittable arguments: https://learn.microsoft.com/dotnet/api/system.runtime.interopservices.unmanagedcallersonlyattribute

## Current refactor readiness

The source tree now establishes the first boundaries needed by this plan:

```text
include/heartstead/core       shared math, types and jobs
include/heartstead/game       gameplay-facing player API
include/heartstead/platform   window/platform services
include/heartstead/render     renderer configuration and public renderer API
include/heartstead/voxel      voxel storage and meshing contracts
include/heartstead/world      world storage, generation and streaming
src/game                      native fallback gameplay implementation
src/platform                  GLFW/OS implementation
src/render/ui                 pixel UI rasterization
src/world                     generation and streaming implementation
```

`PlayerInput` is deliberately independent of GLFW. A future C# tick can produce that same input/intent data without depending on the native window or OpenGL renderer.
