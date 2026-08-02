# Multiplayer chunk-subscription benchmarks

This benchmark measures the authoritative chunk-interest path as a macro workload rather than
timing the planner or snapshot codec in isolation. It creates a real `GameRuntime`, one server, and
multiple in-memory clients; runs the production subscription planner, publication tables, binary
slice codec, reliable FIFO, and client snapshot/removal intake; and records wall-clock time around
`ServerRuntime::run_tick`. Content validation, scenario startup, marker creation, asynchronous
lighting/collision settlement, and client synchronization are outside the server-tick timer.

The default deterministic workload uses eight clients and four phases:

1. All clients move to one distant cluster containing two loaded marker chunks. The server must
   encode each chunk once and reuse it across all eight recipients.
2. Clients move into disjoint regions 128 chunks apart. Each region contains two unique marker
   chunks, so cross-client publication is forbidden and codec work should not appear shared.
3. Each client completes six four-chunk traversal transitions. The stride moves the next marker
   beyond the previous subscribe radius and exercises bounded additions, hysteretic removals, and
   reliable client eviction.
4. The final positions run for 24 steady ticks to retain idle server samples and prove that queues
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

Schema-v1 JSON retains every measured server tick, transition, and per-client traffic row. Tick
rows include:

- P50/P95/P99/max source samples and phase provenance;
- current subscriptions, convergence, bounded additions/removals, and deferred work;
- current/stale/partial publications and atomic snapshot/slice counts;
- codec operations, payload bytes, actual time, overshoot, and time-budget deferrals;
- reliable pending messages/bytes and admission deferrals;
- exact packet-codec wire bytes per client and reliable/unreliable channel;
- client-completed snapshots and applied reliable removals.

The run returns an error rather than producing an apparently valid report if a client disconnects,
a transition misses its timeout, a snapshot becomes partial, final backlog remains, final clients
are not current, raw samples are missing or unordered, or another client's region is present in a
client subscription or resident world. The default evaluated gates are:

| Measure | Gate |
| --- | ---: |
| Server tick P95 | at most 12.5 ms |
| Server tick P99 | at most 16.667 ms |
| Maximum server tick | at most 50 ms |
| Any cluster/spread/traversal convergence | at most 16 ticks |
| Bounded reliable burst recovery | at most 2 ticks |
| Clustered snapshot encode reuse | at least 2.0 recipients/operation |
| Disjoint snapshot encode reuse | at most 1.05 recipients/operation |
| Snapshot codec one-operation overshoot | at most 1,000 us |
| Exact wire bytes/client/tick | at most 320 KiB |
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

## Clean reference calibration

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

## Scope limit

This is a deterministic, in-process chunk-interest benchmark. It does not simulate RTT, jitter,
loss, retransmission, socket fragmentation, malicious clients, the separately committed voxel
event/delta path, dynamic hot-region edits, multi-hour soak, or whole-process memory growth. It
closes the reproducible spread/convergence/traversal and clean-host P99 evidence slice; impaired
network, hot-edit, spatial event filtering, and soak acceptance remain separate M6 work.
