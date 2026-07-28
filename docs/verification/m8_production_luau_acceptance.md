# M8 Production Luau Acceptance

Date: 2026-07-28

Implementation commit: `74bd4c339de4025a68d71a7b6bf8b52216772f06`

## Scope

M8 delivers the production scripting boundary, sandbox, resource enforcement, validation,
inspection, and benchmark infrastructure. Its conformance modules are neutral test fixtures.
It deliberately does not author, port, or modify Age-0 or other gameplay mechanics in
`mods/base`.

## Dependency and boundary

| Item | Accepted value |
| --- | --- |
| vcpkg baseline | `b81cab76a175645e047601da8f982ad86bff1cc3` |
| Luau version | 0.729 |
| Build switch | `HEARTSTEAD_ENABLE_LUAU` |
| Public boundary | `IScriptRuntime` and engine-owned data values |
| Production ownership | one VM per `(source_mod_id, ScriptStage)` |
| Disabled behavior | metadata validation with explicit non-executing backend |

The official Luau Compiler and VM targets are linked privately into `heartstead_engine`.
No Luau state, stack value, bytecode, native closure, pointer, or header type crosses the public
engine boundary.

Source is compiled to bytecode internally and loaded directly into a sandboxed module thread.
There is no public bytecode-loading path. Modules within one mod/stage VM have separate global
environments; different mods and server, client, and migration stages cannot share mutable VM
state.

## Sandbox and limits

The production backend opens Luau's safe standard libraries, freezes the shared environment, and
sandboxes each module thread. It removes filesystem/process-adjacent or introspective entry
points including `debug`, `os`, `require`, `getfenv`, `setfenv`, `loadstring`, `newproxy`, and
`collectgarbage`. Scripts receive only the narrow data-only `emit` host closure.

Every host call revalidates stage, API version, capability, schema, argument bounds, and event
budget. Default production limits are:

- 256 KiB source per module and 256 loaded modules;
- 8 MiB hard allocator ceiling per mod/stage VM;
- 100,000 interrupt-budget units and 50 ms wall time per call;
- 128 stack frames, 32 call arguments, 64 emitted events, 64 KiB boundary strings, and
  4 KiB retained errors.

The allocator rejects growth above the VM ceiling. The interrupt callback applies budget,
deadline, cancellation, and stack checks to the active module and its child coroutines. Error,
yield, interrupt, and allocation-failure paths reset the thread and leave the VM usable.

## Conformance and hostile cases

`heartstead_scripting_runtime_tests` covers:

- neutral runtime-server, runtime-client, and migration API calls;
- mod-to-mod, stage-to-stage, and module-global isolation;
- deterministic module enumeration, unload/reload, and state reset;
- API versions, permissions, schemas, value bounds, and event ordering;
- content fingerprints that include script source and permissions while preserving the legacy
  no-script stream;
- forbidden globals, malformed and oversized source, infinite loops, child-coroutine loops,
  recursion, explicit yields, cancellation, deadlines, allocation bombs, oversized event/value
  output, event floods, and bounded error strings;
- a successful call after every hostile failure, proving recovery rather than process or VM
  poisoning.

Aggregate mod validation also compiles and bounded-initializes discovered modules before content
is accepted when Luau is enabled. Syntax, initialization, export-shape, and resource failures are
reported as structured diagnostics.

## Verification matrix

Machine: Intel Core Ultra 7 258V, 8 physical/logical cores, Linux x86-64 kernel
`6.17.0-1028-oem`. Compilers: GCC 13.3.0 and Clang 18.1.3.

| Configuration | Relevant settings | Result |
| --- | --- | ---: |
| `default-debug-werror` | GCC, Debug, warnings as errors, Luau on | 95 / 95 passed |
| fresh Clang Debug equivalent | Clang, Debug, warnings as errors, Luau on | 95 / 95 passed |
| `linux-clang-asan` | Clang, ASan+UBSan, warnings as errors, Luau on | 95 / 95 passed |
| `linux-clang-tsan` | Clang, TSan, warnings as errors, Luau on | 95 / 95 passed |
| `default-release` | GCC, Release, Luau on | 95 / 95 passed |
| full disabled check | GCC, Debug, warnings as errors, Luau off | 95 / 95 passed |

The disabled check was a fresh full configure and 246-target build with
`HEARTSTEAD_ENABLE_LUAU=OFF`, followed by the complete suite. It includes the engine tests,
scripting runtime tests, scripting benchmark smoke test, scripting sandbox, mod validator, dev
game, and dedicated server rather than testing only the fallback target in isolation.

The sanitizer presets disable Vulkan as configured by the repository and exercise the complete
headless/test graph. The matrix also includes the M1-M7 regression and acceptance fixtures,
binary transport mutation target, headless server smoke test, save/reload paths, and all benchmark
smoke scenes.

## Release benchmark

Command:

```bash
./build/default-release/apps/scripting_benchmark/heartstead_scripting_benchmark \
  --modules 16 --warmup 1000 --calls 10000 \
  --output build/default-release/scripting-benchmark.json
```

| Measurement | Result | Gate |
| --- | ---: | ---: |
| Warm-up calls | 1,000 | — |
| Measured calls | 10,000 | — |
| Modules | 16 | — |
| p50 | 0.006492 ms | — |
| p95 | 0.007999 ms | < 0.25 ms |
| Maximum | 0.040471 ms | — |
| Current/peak VM memory | 383,904 bytes | < configured ceilings |
| Interrupt callbacks | 715,018 | visible |

The neutral benchmark passed its published p95 gate.

## Intentionally deferred validation

The automated M1-M8 implementation and verification gate is complete. M7's physical
two-machine LAN walkthrough remains an explicit release-operator check: this environment has one
host and does not provide the second machine or privileged network shaping needed for that manual
exercise. The deterministic 100 ms round-trip, 2% loss runtime gate, true-socket integration, and
codec mutation tests remain green and are recorded in the
[M7 acceptance report](m7_remote_multiplayer_acceptance.md).
