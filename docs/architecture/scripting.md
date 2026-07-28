# Scripting Architecture

Scripting is an engine-owned sandbox boundary for mod behavior. The engine embeds the official
Luau compiler and VM, but no Luau type, pointer, bytecode, or native closure escapes
`engine/scripting`.

M8 deliberately adds no gameplay or Age-0 behavior. Its conformance modules are test-only and use
neutral values and events. Base-mod gameplay scripting starts after the production boundary is
accepted.

## Backends and ownership

- `ScriptBackend::luau` is the production backend. Luau 0.729 is pinned through the repository's
  vcpkg baseline and linked privately into `heartstead_engine`.
- `ScriptBackend::disabled` validates and stores module metadata but does not execute source. It
  keeps headless tools and builds with `HEARTSTEAD_ENABLE_LUAU=OFF` functional.
- An `IScriptRuntime` owns all VMs and module records. Applications and gameplay own the runtime;
  the engine never keeps process-global script state.
- The production backend creates one VM for each `(source_mod_id, ScriptStage)` pair. Server,
  client, and migration state cannot alias, and different mods never share a VM.
- Modules in one mod/stage VM receive separate sandboxed global environments. They can share the
  frozen standard-library implementation without seeing another module's mutable globals.

The runtime compiles trusted source input to Luau bytecode internally and immediately loads that
bytecode into the destination sandbox. There is no public bytecode loading API, so mods cannot
inject bytecode that bypasses compiler validation.

## Sandbox

VM creation follows Luau's embedding guidance:

1. create the state with the runtime's accounting allocator;
2. open the safe standard libraries;
3. remove `debug`, `getfenv`, `setfenv`, `loadstring`, `newproxy`, `collectgarbage`, `os`, and
   `require`;
4. freeze the global library environment with `luaL_sandbox`;
5. create each module thread and isolate it with `luaL_sandboxthread`;
6. expose only Heartstead's narrow `emit` closure.

No filesystem, process, dynamic-library, raw-network, debugger, or native engine handle is
available. Host calls revalidate the module stage, API version, declared permission, registered API
id, argument schema, value bounds, and per-call event budget. A script therefore produces data-only
`ScriptEmittedEvent` records; it cannot mutate world state directly.

The game-owned host event queue preserves module id, source mod, source path, stage, function,
arguments, API version, sequence, and consumed-budget estimate. The authoritative command
dispatcher remains responsible for transactions, validation, persistence, and replication.

## Module discovery and validation

`ScriptModuleLoader` materializes lifecycle-classified `.lua` and `.luau` files into
`ScriptModuleDesc` records. It:

- derives stable module ids from the owning mod and relative path;
- confines files to the owning mod root and rejects symlinks/forged paths;
- bounds source, module, directive, and permission counts before runtime load;
- maps files to runtime-server, runtime-client, or migration stage;
- parses `-- heartstead.permissions = "..."` and `-- heartstead.api_version = "..."` metadata,
  replacing directive bytes with spaces to retain source line offsets.

When Luau is enabled, aggregate mod validation compiles and bounded-initializes every discovered
module. Syntax errors, initialization failures, non-table exports, resource-limit failures, and
other VM errors become `mod.scripting.module_invalid` diagnostics before content is accepted.

Mod compatibility fingerprints include each module's stable id, stage, API version, source,
and sorted permission set. Input/module/permission ordering does not affect the fingerprint, while
source or capability changes do.

## Boundary values and APIs

The public script value ABI is intentionally small: nil, boolean, finite number, and bounded
string. Modules return a readonly table of exported functions. Dotted export lookup is supported
for nested readonly tables; argument arity is checked before the call.

`ScriptHostApiDesc` supplies:

- a stable lowercase dotted API/event id;
- owning stage and minimum module API version;
- required capabilities;
- a named, ordered argument schema with trailing optional values.

Loader metadata grants only declared `ScriptPermission` values. Every host API invocation checks
those grants again. Registering an API does not grant it.

## Resource limits and recovery

The default production limits are:

- 256 KiB source per module and 256 loaded modules;
- 8 MiB for each mod/stage VM;
- 100,000 interrupt-budget units per call;
- 50 ms wall time and 128 stack frames per call;
- 32 call arguments, 64 emitted events, 64 KiB boundary strings, and 4 KiB errors.

The VM's custom reallocator tracks current and peak memory and refuses growth above the hard
ceiling. Luau's interrupt callback checks host cancellation, budget, stack depth, and deadline.
All failures unwind the module stack; allocation failures additionally run a full collection after
unreachable call state is removed. A hostile call can therefore fail without poisoning the VM for
the next call.

`ScriptRuntimeStats` exposes VM/module counts, current/peak/limit memory, source/bytecode totals,
calls/failures, interrupts, emitted events, and call timings through debug inspection.

## Verification and benchmark

`heartstead_scripting_runtime_tests` proves:

- runtime-server, runtime-client, migration, mod-to-mod, and module-global isolation;
- deterministic enumeration, unload/reload, module-state reset, and script fingerprints;
- permission/API/schema enforcement and bounded value/event/error output;
- forbidden globals, malformed source, infinite loops, recursion, cancellation, deadlines, and
  allocation bombs fail closed;
- the same VM remains usable after every hostile fixture.

`heartstead_scripting_benchmark` loads neutral modules and measures calls that perform fixed Luau
arithmetic without gameplay or host mutation. The published Release target is below `0.25 ms` p95
per call:

```bash
cmake --build --preset default-release --target heartstead_scripting_benchmark
./build/default-release/apps/scripting_benchmark/heartstead_scripting_benchmark \
  --modules 16 --warmup 1000 --calls 10000 \
  --output build/default-release/scripting-benchmark.json
```

The JSON records p50/p95/maximum call latency, module/call counts, current/peak VM memory,
interrupt count, the target, and whether the run met it. With Luau disabled it emits an explicit
`available: false` record and exits successfully, keeping the disabled build verifiable.
