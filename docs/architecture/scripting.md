# Scripting architecture

Scripting is an engine-owned sandbox boundary for mod behavior. The production backend embeds the
official Luau compiler and VM, but no Luau type, pointer, bytecode, native closure, or engine handle
escapes `engine/scripting`.

Gameplay scripts use this boundary only after content discovery and validation. The engine also
supports a disabled backend so metadata, tools, headless configurations, and compatibility checks
remain functional when Luau execution is compiled out.

## Backends and ownership

- `ScriptBackend::luau` is the production execution backend. The exact Luau revision is pinned by
  the repository's vcpkg manifest/baseline rather than duplicated here.
- `ScriptBackend::disabled` validates and stores module metadata but does not execute source.
- An application/game-owned `IScriptRuntime` owns all VMs and module records; there is no required
  process-global VM.
- The production backend creates one VM per `(source_mod_id, ScriptStage)` pair. Runtime-server,
  runtime-client, and migration state cannot alias, and different mods do not share a VM.
- Modules inside one VM receive separate sandboxed global environments. They may share frozen
  standard-library implementation without observing another module's mutable globals.

Trusted source is compiled internally and loaded directly into the destination sandbox. There is no
public bytecode-loading API for mods.

## Sandbox

VM setup follows this sequence:

1. create the state with the runtime's accounting allocator;
2. open only safe standard libraries;
3. remove `debug`, `getfenv`, `setfenv`, `loadstring`, `newproxy`, `collectgarbage`, `os`, and
   `require`;
4. freeze the global library environment;
5. create and isolate each module thread;
6. expose only registered, narrow Heartstead host closures.

Scripts receive no filesystem, process, dynamic-library, raw-network, debugger, Vulkan, or native
engine access. Host calls revalidate stage, module API version, declared permission, registered API
ID, argument schema, value bounds, and per-call event budget.

A script emits bounded data-only records. It cannot mutate `WorldState` directly. Game-owned code
converts allowed events into normal commands or other validated host behavior, preserving
transactions, authority, persistence, and replication.

## Module discovery and validation

`ScriptModuleLoader` materializes lifecycle-classified `.lua` and `.luau` files into stable module
descriptors. Discovery:

- derives IDs from owning mod and normalized relative path;
- confines files to the mod root and rejects symlinks or forged paths;
- bounds source, module, directive, and permission counts before execution;
- maps files to runtime-server, runtime-client, or migration stages;
- parses `heartstead.permissions` and `heartstead.api_version` directives while retaining source
  line offsets for diagnostics.

With Luau enabled, aggregate mod validation compiles and bounded-initializes every discovered
module. Syntax errors, initialization failures, invalid exports, resource-limit failures, and VM
errors reject the affected content with structured diagnostics before it becomes active.

Compatibility fingerprints include stable module ID, stage, API version, source, and sorted
permission set. Enumeration order does not change the fingerprint; source or capability changes do.

## Boundary values and host APIs

The public value ABI is intentionally small: nil, boolean, finite number, and bounded string.
Modules export a readonly table of functions. Nested readonly tables support dotted lookup, and
arity is checked before invocation.

A host API descriptor supplies:

- a stable lowercase dotted API/event ID;
- owning stage and minimum module API version;
- required permissions/capabilities;
- a named ordered argument schema with trailing optional values.

Registering an API does not grant it. Loader metadata grants only explicitly declared permissions,
and every invocation checks them again.

## Resource limits and recovery

Default production limits are:

- 256 KiB source per module and 256 loaded modules;
- 8 MiB per mod/stage VM;
- 100,000 interrupt-budget units per call;
- 50 ms wall time and 128 stack frames per call;
- 32 arguments, 64 emitted events, 64 KiB boundary strings, and 4 KiB error text.

The accounting allocator tracks current and peak memory and refuses growth beyond the hard limit.
The interrupt callback checks host cancellation, budget, stack depth, and deadline. Failures unwind
the module stack; allocation failures also collect unreachable call state. A hostile or defective
call can therefore fail closed without permanently poisoning the VM.

`ScriptRuntimeStats` exposes VM/module counts, current/peak/limit memory, source/bytecode totals,
calls/failures, interrupts, emitted events, and timings through debug inspection.

## Verification and benchmark

`heartstead_scripting_runtime_tests` covers stage/mod/module isolation, deterministic discovery,
unload/reload, state reset, fingerprints, permission/API/schema enforcement, boundary limits,
forbidden globals, malformed source, loops, recursion, cancellation, deadlines, allocation bombs,
and VM reuse after failure.

The scripting benchmark loads neutral modules and measures fixed Luau arithmetic without gameplay
or host mutation. The maintained Release target is below `0.25 ms` p95 per call:

```bash
cmake --build --preset default-release --target heartstead_scripting_benchmark
./build/default-release/apps/scripting_benchmark/heartstead_scripting_benchmark \
  --modules 16 --warmup 1000 --calls 10000 \
  --output build/default-release/scripting-benchmark.json
```

The output records latency percentiles, module/call counts, memory, interrupts, target, and result.
With Luau disabled, it emits an explicit unavailable result and exits successfully so the disabled
configuration remains verifiable.

See [Testing](../dev/testing.md) for the broader change-verification matrix.
