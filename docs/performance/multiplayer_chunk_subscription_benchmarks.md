# Multiplayer chunk-subscription benchmarks

This benchmark measures the authoritative chunk-interest path as a macro workload rather than
timing the planner or snapshot codec in isolation. It creates a real `GameRuntime`, one server, and
multiple in-memory clients; runs the production subscription planner, publication tables, binary
slice codec, reliable FIFO, command transaction gateway, spatial relevance filter, typed voxel
delta path, and client snapshot/removal/delta intake; and records wall-clock time around
`ServerRuntime::run_tick`. Content validation, scenario startup, marker creation, asynchronous
lighting/collision settlement, message delivery, and client synchronization are outside the
server-tick timer.

The default deterministic workload uses eight clients and five phases:

1. All clients move to one distant cluster containing two loaded marker chunks. The server must
   encode each chunk once and reuse it across all eight recipients.
2. Clients move into disjoint regions 128 chunks apart. Each region contains two unique marker
   chunks, so cross-client publication is forbidden and codec work should not appear shared.
3. Each client completes six four-chunk traversal transitions. The stride moves the next marker
   beyond the previous subscribe radius and exercises bounded additions, hysteretic removals, and
   reliable client eviction.
4. Clients move to eight non-overlapping regions inside one fixed physics island. For 120 measured
   ticks, every client submits one real `world.set_voxel` command that alternates a boundary cell
   between clay and stone. Each tick must retain eight command results, eight relevant event/delta
   deliveries, 56 filtered cross-region pairs, eight exact client applies, no full chunk snapshot,
   no fluid activation, and no reliable backlog.
5. The final positions run for 24 steady ticks to retain idle server samples and prove that queues
   remain drained.

The reliable delivery limit is deliberately 48 messages/client/tick. Two complete 32-slice
snapshots therefore form a bounded burst that cannot drain in its admission tick. This makes the
backlog-recovery gate observable. Marker chunks are created before the measured interval and all
unrelated lighting, collision, and bootstrap work must remain settled for three consecutive ticks.

## Serialization admission

Ordinary server ticks have a 4,000 us global chunk-snapshot serialization budget. Cache hits remain
eligible after the boundary because they add no codec work; a cache miss is deferred once measured
work has reached the boundary. The non-preemptible codec operation that crosses the boundary may
finish, and its exact one-operation overshoot is retained. Rotating client and snapshot cursors
prevent one stable client identity from owning the deferral indefinitely.

Direct in-memory startup keeps its existing synchronous bootstrap exception. It still obeys the
reliable backlog caps, but it does not consume an ordinary server tick's codec quota. Socket-backed
and already-published connections use incremental ordinary-tick admission. A transport welcome is
therefore not equivalent to gameplay readiness: callers that need prediction state must also wait
for the local authoritative player snapshot.

## Evidence and fail-closed behavior

Schema-v2 JSON retains every measured server tick, transition, and per-client traffic row. Tick
rows include:

- P50/P95/P99/max source samples and phase provenance;
- current subscriptions, convergence, bounded additions/removals, and deferred work;
- current/stale/partial publications and atomic snapshot/slice counts;
- codec operations, payload bytes, actual time, overshoot, and time-budget deferrals;
- reliable pending messages/bytes and admission deferrals;
- exact packet-codec wire bytes per client and reliable/unreliable channel;
- client-completed snapshots, applied reliable removals, command results, event/delta messages, and
  applied voxel edits;
- raw scheduler total plus named per-system time, including `runtime.command_gateway`;
- fluid topology/dirty-collection time and active/processed cells;
- spatial delivered/filtered pairs and delta-advanced/avoided/gap publication counts.

The run returns an error rather than producing an apparently valid report if a client disconnects,
a transition misses its timeout, a snapshot becomes partial, final backlog remains, final clients
are not current, raw samples are missing or unordered, or another client's region is present in a
client subscription or resident world. Hot-edit samples additionally fail if a transaction reports
derived work other than the expected mesh rebuild, a recipient receives foreign-region state, a
delta is not contiguous, a redundant full snapshot appears, fluid work activates, or exact
identity/revision/cell convergence is lost. The default evaluated gates are:

| Measure | Gate |
| --- | ---: |
| Server tick P95 | at most 12.5 ms |
| Server tick P99 | at most 16.667 ms |
| Maximum server tick | at most 50 ms |
| Hot-edit server tick P95 | at most 12.5 ms |
| Hot-edit server tick P99 | at most 16.667 ms |
| Maximum hot-edit server tick | at most 50 ms |
| Any cluster/spread/traversal/hot-edit convergence | at most 16 ticks |
| Bounded reliable burst recovery | at most 2 ticks |
| Clustered snapshot encode reuse | at least 2.0 recipients/operation |
| Disjoint snapshot encode reuse | at most 1.05 recipients/operation |
| Snapshot codec one-operation overshoot | at most 1,000 us |
| Exact wire bytes/client/tick | at most 320 KiB |
| Exact hot-edit wire bytes/client/tick | at most 2 KiB |
| Subscription/addition/removal/partial/disconnect/final-backlog limits | no violation |

## Commands

Build and run an optimized, gate-enforced report:

```bash
cmake --preset default-release
cmake --build build/default-release \
  --target heartstead_multiplayer_chunk_subscription_benchmark -j2

build/default-release/apps/multiplayer_chunk_subscription_benchmark/\
heartstead_multiplayer_chunk_subscription_benchmark \
  --enforce-gates \
  --output build/default-release/benchmarks/multiplayer-chunk-subscriptions.json
```

Run the small two-client contract test:

```bash
cmake --build build/default-release \
  --target heartstead_multiplayer_chunk_subscription_benchmark_tests -j2
build/default-release/tests/heartstead_multiplayer_chunk_subscription_benchmark_tests
```

Use `--help` for workload and gate overrides. Raw reports belong under the ignored build tree, not
in Git. Compare independent optimized processes with the same commit, build configuration, seed,
content, and machine.

## Schema-v1 clean snapshot reference

On 2026-08-01, three independent Release processes from clean commit
`bc4b16c9aad56540ec60ca1bfd5e622ac772be17` passed every gate and fail-closed invariant on an Intel
Core Ultra 7 258V with GCC 13.3.0 on Linux 6.17.0-1030-oem. Each process recorded 98 measured server
ticks, `git_dirty=false`, eight clients, six traversal steps, and the default seed and budgets.

| Process | Tick P50 | Tick P95 | Tick P99/max | Codec max | Codec overshoot | Time deferrals |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.061 ms | 2.495 ms | 4.747 ms | 4,027 us | 27 us | 1 |
| 2 | 0.065 ms | 2.579 ms | 4.862 ms | 4,099 us | 99 us | 0 |
| 3 | 0.052 ms | 2.505 ms | 4.526 ms | 4,025 us | 25 us | 1 |
| **Median** | **0.061 ms** | **2.505 ms** | **4.747 ms** | **4,027 us** | **27 us** | **1** |

Behavioral evidence was identical across all three processes: cluster and spread transitions took
10 ticks; every traversal step took 9 ticks; the forced reliable backlog recovered in 1 tick;
clustered encoding reused each operation across 8 recipients while disjoint encoding remained
1-to-1; all 112 cross-region exclusions were verified; and no client disconnected or ended with
pending, stale, or partial state. Peak exact wire traffic was 9,327 bytes/client/tick. The workload
delivered 511,780 reliable wire bytes in total and no unreliable bytes because it issues no movement
inputs or changing entity-motion state.

## Schema-v2 clean hot-edit calibration

On 2026-08-02, three independent Release processes from clean commit
`b2d4bdeb414540ffe96a1511c3c38f07ca3c9e4e` passed every gate and fail-closed invariant on the same
Intel Core Ultra 7 258V, GCC 13.3.0, and Linux 6.17.0-1030-oem reference host. Each process recorded
228 measured server ticks, `git_dirty=false`, eight clients, 120 hot-edit ticks, 960 committed edit
commands, 6,720 verified foreign-region exclusions, and the default seed and budgets.

| Process | Overall P95 | Overall P99/max | Hot P95 | Hot P99/max | Command gateway average/max |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.441 ms | 2.569/4.647 ms | 0.424 ms | 0.441/0.503 ms | 0.296/0.404 ms |
| 2 | 0.905 ms | 2.580/4.784 ms | 0.776 ms | 0.984/1.010 ms | 0.317/0.850 ms |
| 3 | 0.415 ms | 2.539/4.764 ms | 0.406 ms | 0.461/0.616 ms | 0.291/0.501 ms |
| **Median** | **0.441 ms** | **2.569/4.764 ms** | **0.424 ms** | **0.461/0.616 ms** | **0.296/0.501 ms** |

Behavioral evidence was identical across all processes. All 960 relevant voxel deltas advanced the
recipient's complete publication from the preceding revision and avoided all 960 otherwise
redundant 32-slice snapshots; there were zero publication gaps. Every hot tick applied exactly one
edit per interested client, filtered the other seven regions, retained a mesh-only derived-update
trace, left fluid active/processed work at zero, and drained reliable output in the same tick. Peak
hot traffic was 860 bytes/client/tick, below the calibrated 2 KiB gate.

The optimization was selected from raw per-system evidence. A controlled dirty-worktree diagnostic
before copy-on-write staging measured hot P95/P99/max at 21.429/22.950/24.405 ms and
`runtime.command_gateway` at 20.271 ms average. The same diagnostic after the change measured
0.446/0.795/0.823 ms and 0.306 ms average, while wire traffic and the 960/960/0 delta
advanced/avoided/gap counts stayed identical. The cause was a deep copy of every resident chunk's
32³ cell field for every strong-rollback command transaction. `VoxelChunk` now shares that immutable
dense field across staged `WorldState` copies and detaches only the chunk being written. Rollback
semantics remain unchanged and are covered by an intentional post-mutation command failure test.

Material-equivalent edits also use palette-derived dependency keys: clay/stone dirties mesh and
save/replication state, but not collision, lighting, or the fluid frontier. Fluid dirty-region
activation clips iteration to the exact resident intersection rather than scanning all 32³ cells in
every intersecting chunk. These reductions are asserted by the hot workload and focused engine/fluid
tests rather than inferred only from timing.

## Spatial voxel-event contract

The schema-v2 macrobenchmark now exercises sustained edit delivery directly. A separate
deterministic two-client runtime test retains the late-interest fallback contract. Both clients begin
with the edited chunk published; one is moved 100 chunks away until its subscription removal has
reached the client. A real authoritative remove-voxel command must then produce one spatial event,
one relevant event-recipient pair, one filtered pair, no immediate event batch or typed delta at the
far client, and one accepted edit at the near client. The near client's contiguous delta must
advance its publication without a full snapshot. Returning the far client must recover the edited
cell exactly from a current identity/revision chunk snapshot.

Run that contract in the Debug/Werror configuration with:

```bash
cmake --build build/default-debug-werror \
  --target heartstead_server_chunk_subscription_tests -j2
ctest --test-dir build/default-debug-werror --output-on-failure \
  -R '^heartstead_server_chunk_subscription_tests$'
```

## Scope limit

This is a deterministic, in-process chunk-interest benchmark. It does not simulate RTT, jitter,
loss, retransmission, socket fragmentation, malicious clients, multi-hour soak, or whole-process
memory growth. It closes the reproducible spread/convergence/traversal, isolated hot-region edit,
spatial relevance, and clean-host P99 evidence slices. Impaired-network and soak acceptance remain
M6 work; client ordering when an older voxel delta and a newer full chunk snapshot are both queued
is tracked separately from this contiguous-delta workload.
