# Multiplayer chunk-subscription benchmarks

This benchmark measures the authoritative chunk-interest path as a macro workload rather than
timing the planner or snapshot codec in isolation. It creates a real `GameRuntime`, one server, and
multiple in-memory clients; runs the production subscription planner, publication tables, binary
slice codec, reliable FIFO, command transaction gateway, spatial relevance filter, typed voxel
delta path, and client snapshot/removal/delta intake; and records wall-clock time around
`ServerRuntime::run_tick`. Content validation, scenario startup, marker creation, asynchronous
lighting/collision settlement, message delivery, and client synchronization are outside the
server-tick timer.

The default deterministic workload uses eight clients and five foreground phases followed by a
conditioned soak:

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
6. Before memory sampling, 256 alternating edit ticks drive bounded client command histories
   through their cap while restoring the original voxel state, then eight unmeasured traversal/edit
   cycles condition allocator reuse. The 64 measured cycles each move all clients from the hot-edit
   regions to their disjoint spread regions and back, then apply two exact edits that restore the
   same persistent voxel state. The default produces 1,408 measured soak ticks and 65 comparable
   cycle-endpoint samples including the baseline.

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

Schema-v3 JSON retains every foreground server tick, transition, and per-client traffic row in
`raw_ticks`. Tick rows include:

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

The soak path retains every server-tick duration in the compact `soak_tick_times_us` array and
retains one `soak_samples` resource row at the baseline and after every measured cycle. It processes
the larger nested tick state online instead of retaining a second copy of every per-client row.
Both arrays are allocated and touched before the resource baseline, so report construction does not
manufacture the working-set trend being measured. Cycle rows retain logical server and client
ownership, settled queue depth/bytes, thread/open-file counts, and precise process RSS, proportional
set size (PSS), and private resident memory when the host provides them.

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
| Soak server tick P95 | at most 12.5 ms |
| Soak server tick P99 | at most 16.667 ms |
| Maximum soak server tick | at most 50 ms |
| Any cluster/spread/traversal/hot-edit convergence | at most 16 ticks |
| Any measured soak transition convergence | at most 16 ticks |
| Bounded reliable burst recovery | at most 2 ticks |
| Measured soak reliable burst recovery | at most 2 ticks, with at least one observed burst |
| Clustered snapshot encode reuse | at least 2.0 recipients/operation |
| Disjoint snapshot encode reuse | at most 1.05 recipients/operation |
| Snapshot codec one-operation overshoot | at most 1,000 us |
| Exact wire bytes/client/tick | at most 320 KiB |
| Exact hot-edit wire bytes/client/tick | at most 2 KiB |
| Soak client edit application and foreign-region exclusions | exactly the expected totals |
| Soak partial snapshots/stale publications/disconnects | zero |
| Peak and final logical server/client ownership | no growth from the comparable baseline |
| Settled soak queue depth and bytes | zero at every sampled cycle endpoint |
| Thread and open-file growth | no positive growth |
| Private resident-memory ordinary-least-squares slope | at most 65,536 bytes/cycle |
| Private resident-memory final-minus-baseline growth | at most 8,388,608 bytes |
| Subscription/addition/removal/partial/disconnect/final-backlog limits | no violation |

The memory gates run only when every cycle has precise accounting. Maintained calibration commands
add `--require-precise-memory`, which fails closed if it is unavailable. On Linux, the ordinary F3
sampler deliberately keeps the cheap `/proc/self/statm` RSS path, while this sparse benchmark path
reads `/proc/self/smaps_rollup`. The Linux kernel documents that `statm` RSS is asynchronously
maintained and imprecise, recommends `smaps` when accuracy matters, and defines `smaps_rollup` as
the pre-summed mapping data ([`/proc` documentation](https://www.kernel.org/doc/html/v6.15/filesystems/proc.html),
[`smaps_rollup` ABI](https://www.kernel.org/doc/html/v7.0/admin-guide/abi-testing.html)). Here,
private resident memory means `Private_Clean + Private_Dirty`; it is not committed virtual memory or
allocator ownership.

The slope is the ordinary least-squares line through cycle index and private resident bytes, using
the standard linear least-squares model described by NIST
([model](https://www.itl.nist.gov/div898/handbook/pmd/section1/pmd141.htm),
[estimation](https://www.itl.nist.gov/div898/handbook/pmd/section4/pmd431.htm)). It is a descriptive
bounded trend, not a statistical-significance claim. The separate endpoint-growth gate catches a
late step that a fitted slope could dilute.

## Commands

Build and run an optimized, gate-enforced report:

```bash
cmake --preset default-release
cmake --build build/default-release \
  --target heartstead_multiplayer_chunk_subscription_benchmark -j2

build/default-release/apps/multiplayer_chunk_subscription_benchmark/\
heartstead_multiplayer_chunk_subscription_benchmark \
  --enforce-gates \
  --require-precise-memory \
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

### Post-ordering clean validation

After revision-safe mixed snapshot/delta intake was added, three independent clean Release
processes at commit `5cd11bb3a1536d33821232dd425ae76f6811ef3b` reran the complete schema-v2
workload on the same reference host. Every process retained `git_dirty=false`, all 960 exact edits,
all 6,720 exclusions, 960 advanced/avoided publications, zero gaps, 860 peak bytes/client/tick, and
zero gate violations.

| Process | Overall P95 | Overall P99/max | Hot P95 | Hot P99/max |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 0.436 ms | 2.472/4.477 ms | 0.399 ms | 0.436/0.504 ms |
| 2 | 0.388 ms | 2.482/4.752 ms | 0.374 ms | 0.409/0.497 ms |
| 3 | 0.390 ms | 2.564/4.829 ms | 0.381 ms | 0.394/0.538 ms |
| **Median** | **0.390 ms** | **2.482/4.752 ms** | **0.381 ms** | **0.409/0.504 ms** |

The workload's contiguous hot path is a performance-regression guard, not the proof of mixed-queue
ordering behavior. Focused client transport tests establish that proof: a base snapshot and next
delta apply in one synchronization; an older delta cannot regress a newer complete snapshot; two
same-cell deltas advance one revision at a time; and a forward gap fails without advancing the
client cursor, after which a contiguous edit can recover. Typed deltas also reject a global event
payload that differs from the paired observed event batch.

## Schema-v3 clean conditioned-soak calibration

On 2026-08-02, three sequential independent Release processes from clean commit
`1b1db1b673682d9f3f85b13b8df0f57b1c7a846c` passed every gate with
`--require-precise-memory` on the same Intel Core Ultra 7 258V, GCC 13.3.0, and Linux
6.17.0-1030-oem reference host. Each process retained `git_dirty=false`, 228 foreground ticks,
1,408 measured soak ticks, 64 measured cycles, and the default conditioning and gate profile.
Times below are milliseconds. The RSS and private-resident columns show one byte value because
baseline, final, and peak were identical within each process.

| Process | Foreground P50/P95/P99/max | Hot-edit P50/P95/P99/max | Soak P50/P95/P99/max | Peak reliable messages/bytes | Precise RSS/private resident bytes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.356/0.431/2.535/4.697 | 0.362/0.407/0.438/0.531 | 0.061/2.627/4.734/4.839 | 128/24,832 | 84,373,504/80,470,016 |
| 2 | 0.366/0.429/2.705/4.656 | 0.378/0.414/0.429/0.566 | 0.063/2.691/4.751/4.846 | 112/21,728 | 84,488,192/80,564,224 |
| 3 | 0.359/0.444/2.703/4.729 | 0.367/0.417/0.452/0.687 | 0.062/2.686/4.709/4.841 | 128/24,832 | 84,443,136/80,539,648 |
| **Median** | **0.359/0.431/2.703/4.697** | **0.367/0.414/0.438/0.566** | **0.062/2.686/4.734/4.841** | **128/24,832** | **84,443,136/80,539,648** |

Behavioral evidence was exact in all three processes. Each measured soak applied both edits in all
eight clients for 1,024 of 1,024 verified states, excluded all seven foreign regions for 7,168 of
7,168 pairs, converged every transition within 10 ticks, observed 64 reliable backlog bursts, and
recovered every burst within two ticks. Partial snapshots, stale publications, and disconnects
remained zero.

The baseline, maximum across all 64 measured endpoints, and final logical ownership were identical:
76 authoritative chunks, zero retained authoritative voxel-edit records, eight total client chunks,
and 2,144 total client-owned record units. The latter is an allocation-free sum of each client's
chunk, remote revision, partial snapshot, command-history, movement, interpolation, entity,
equipment, prediction input, tombstone, and accepted-edit record counts; it is a logical ownership
guard, not a byte-size estimate. Settled queue depth/bytes and thread/open-file growth were zero.
Precise private resident memory had zero endpoint growth and a zero OLS slope in every process.

This accepts the deterministic multi-client queue/private-memory soak slice. The three clean runs
are process repetitions of a compact fixed-endpoint workload, not evidence of multi-hour
stability or a significance test.

## Spatial voxel-event contract

The schema-v3 macrobenchmark exercises sustained edit delivery directly. A separate
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
loss, retransmission, real sockets, socket fragmentation, malicious clients, multi-hour execution,
native GPU/presentation behavior, or physical-display response. Precise private RSS does not
attribute allocator/heap owners, shared memory, device memory, or total virtual commitments. The
64-cycle result establishes a bounded regression workload rather than a statistical assertion
about an unbounded process lifetime.

The benchmark closes the reproducible spread/convergence/traversal, isolated hot-region edit,
spatial relevance, clean-host P99, revision-safe client ordering, and deterministic queue/private-
memory soak slices. The separate
[multiplayer network-impairment benchmark](multiplayer_network_impairment_benchmarks.md) closes the
maintained deterministic single-client 100 ms RTT / 2% unreliable-loss profile. Multi-client
impairment and game-specific temporal aggregation remain M6 work.
