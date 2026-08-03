# Multiplayer network-impairment benchmarks

This benchmark measures the maintained prediction and replication path under deterministic
multi-client network impairment. It creates one real headless `GameRuntime`, its authoritative
`ServerRuntime`, and eight production `ClientRuntime` instances over the in-memory transport. Each
client submits one movement input on every one of 600 measured 60 Hz ticks while the workload
exercises input-bundle redundancy, unreliable loss, prediction, authoritative movement, transient
replication, reconciliation, chunk interest, transport maintenance, and client synchronization.

The default stimulus is:

- eight clients moving in distinct cardinal or diagonal directions;
- 50 ms configured one-way latency, or 100 ms nominal RTT;
- uniform configured delay variation of plus or minus 10 ms;
- 2% loss applied only to unreliable messages;
- one retained 64-bit seed for the shared impairment stream;
- 600 measured ticks per client, with the final 30 inputs neutral so delayed state can settle;
- at most 32 retained recovery ticks after the measured interval.

Warmup scales with the maximum configured delivery delay. It covers connection, assignment, and
subscription delivery windows, then requires a stable interval in which every client is connected
and has local-player state, subscription publication has converged without stale/deferred/partial
work, chunk-loading and streaming queues are settled, the reliable application FIFO is empty, and
aggregate and per-client impairment depth are bounded. A configuration whose warmup timeout cannot
cover those delivery windows is rejected before the workload starts.

Each measured client input has a unique sequence. The recovery interval creates no new prediction
input; it may resend the retained final redundant bundle until every authoritative sequence and
client snapshot reaches 600. Recovery completes only when every prediction buffer and reliable
application queue is empty and every client position is within 0.10 m of authoritative state. The
benchmark fails instead of producing a partial report if this does not happen inside 32 ticks.

## Measurement terminology

[RFC 5481](https://www.rfc-editor.org/rfc/rfc5481.html) describes packet-delay-variation metrics as
derived from measured one-way delays. This in-process workload does not timestamp packet arrivals
as an IPPM measurement, so the report calls the configured uniform plus-or-minus parameter
`simulated_delay_variation_ms`; it does not claim to measure RFC IPDV.

[RFC 7680](https://www.rfc-editor.org/rfc/rfc7680.html) defines a sample loss ratio from losses over
the sampled packet population. The transport therefore reports an explicit
`impairment_eligible_unreliable_message_count` beside simulated drops, globally and for every
client. Observed loss is drops divided by that eligible population, never by a mixed
reliable/unreliable message count. Linux
[`tc-netem(8)`](https://man7.org/linux/man-pages/man8/tc-netem.8.html) likewise separates delay
variation, loss, and a reproducibility seed; Heartstead's simulator is its own uniform,
deterministic implementation and does not claim netem's distributions or kernel timing.

Wire-byte counters are encoded offered load. The in-memory transport charges a message before its
simulated unreliable-loss decision, so dropped traffic remains visible in bandwidth demand. This
is deliberately stricter than counting only delivered bytes.

## Measured and recovery evidence

Schema-v2 JSON retains source/build/CPU provenance, exact stimulus and gate configuration, a
summary, all gate violations, 600 measured tick rows, and a separate bounded recovery-tick array.
Every tick contains sorted per-client rows that exactly reconcile to aggregate prediction,
correction, byte, message, eligible-loss, simulated-drop, connection, and impairment-depth
counters. Report validation rejects a missing, duplicate, reordered, regressing, or
non-reconciling client row.

Measured rows determine server/runtime timing distributions, accepted-input progress, encoded
bandwidth, rolling one-second traffic, and observed loss. Recovery rows do not contaminate those
distributions. They do contribute reconciliation/correction work, transport-integrity failures,
reliable/impairment queue evidence, recovery-server maximum, and final convergence state.

The summary adds nearest-rank P50/P95/P99/max timing, aggregate and per-client correction
distributions, measured authoritative progress, average and rolling traffic, eligible-loss ratios,
peak/final queue depths, final sequences and prediction-buffer ownership, displacement, and final
client/server position error. The default evaluated gates are:

| Measure | Aggregate gate | Per-client gate |
| --- | ---: | ---: |
| Server tick P95 / P99 / max | at most 12.5 / 16.667 / 50 ms | — |
| Maximum recovery server tick | at most 50 ms | — |
| Accepted unique inputs in measured interval | more than 90% of 4,800 | — |
| Authoritative sequence progress at measured boundary | — | more than 90% |
| Hard corrections | at most 8 | at most 1 |
| Maximum correction distance | less than 1 m | retained |
| Final client/server position error | at most 0.10 m | at most 0.10 m |
| Average encoded server-to-client offered load | less than 1,536 KiB/s | less than 192 KiB/s |
| Rolling one-second encoded server-to-client offered load | less than 1,536 KiB | less than 192 KiB |
| Loss-eligible unreliable messages / simulated drops | at least 8 / at least 8 | at least 1 / at least 1 |
| In-flight impaired messages | at most 1,024 | at most 128 |
| Authoritative displacement | minimum retained | more than 5 m |
| Final authoritative/client sequence | minimum exactly 600 | exactly 600 |
| Final unacknowledged prediction inputs | zero | zero |
| Reliable application backlog at final recovery boundary | zero messages and bytes | — |
| Rejected input, reliable drop, malformed/rejected/rate-limited traffic, budget drop, disconnect | zero | — |
| Final connection state | all connected | connected |

The impairment queue is not required to become zero while the server continuously emits movement
snapshots. Its raw final value and peak are retained and bounded. The separate reliable application
FIFO must be empty at the final recovery boundary.

## Commands

Build and run an optimized, gate-enforced report:

```bash
cmake --preset default-release
cmake --build build/default-release \
  --target heartstead_multiplayer_network_impairment_benchmark -j2

build/default-release/apps/multiplayer_network_impairment_benchmark/\
heartstead_multiplayer_network_impairment_benchmark \
  --enforce-gates \
  --output build/default-release/benchmarks/multiplayer-network-impairment.json
```

Run the smaller two-client, 120-tick contract test:

```bash
cmake --build build/default-debug-werror \
  --target heartstead_multiplayer_network_impairment_benchmark_tests -j2
ctest --test-dir build/default-debug-werror --output-on-failure \
  -R '^heartstead_multiplayer_network_impairment_benchmark_tests$'
```

Use `--help` for client count, stimulus, recovery, and aggregate/per-client gate overrides. Raw
reports belong under the ignored build tree, not in Git. Compare independent optimized processes
with the same commit, build configuration, seed, content, and machine.

## Clean reference calibration

On 2026-08-02, three independent Release processes from clean commit
`32463a688cd069de936b2ec0dbf6160157b2b982` passed every gate and report invariant on an Intel Core
Ultra 7 258V with GCC 13.3.0 on Linux 6.17.0-1030-oem. Every report retained
`git_dirty=false`, the default seed, a 28-tick warmup, 600 measured ticks, and 9 recovery ticks.

| Process | Server P50 | Server P95 | Server P99/max | Runtime-frame P99/max | Peak impaired aggregate/client |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.163 ms | 0.198 ms | 0.266/1.302 ms | 1.045/1.811 ms | 682/88 |
| 2 | 0.162 ms | 0.183 ms | 0.190/0.209 ms | 0.868/0.983 ms | 185/27 |
| 3 | 0.165 ms | 0.184 ms | 0.188/0.198 ms | 0.877/0.912 ms | 185/27 |
| **Median** | **0.163 ms** | **0.184 ms** | **0.190/0.209 ms** | **0.877/0.983 ms** | **185/27** |

The first process retained the legitimate asynchronous chunk-publication branch that motivated the
1,024 aggregate / 128 per-client impairment-depth caps; it still remained well inside them.
Behavioral and traffic evidence across the three processes was:

- 4,781-4,782 of 4,800 inputs were accepted inside the measured interval (99.604-99.625%);
  minimum per-client measured authoritative progress was 99.167%, and all authoritative and client
  sequences reached exactly 600 after recovery;
- all 4,800 inputs were predicted, every prediction buffer ended empty, there were zero hard
  corrections, and the maximum soft correction distance was 0.07501 m;
- 42,976-42,984 unreliable messages were loss-eligible and 852 were dropped, an observed ratio of
  1.982-1.983%; every client retained at least 5,372 eligible messages and 93 simulated drops;
- aggregate encoded server-to-client offered load averaged 1,347,076.5-1,357,186.6 bytes/s and
  peaked at 1,358,016-1,454,029 bytes in a rolling second; the maximum per-client average/rolling
  values were 169,652.5/181,795 bytes;
- aggregate impairment depth peaked at 185-682 and ended at 156-159; per-client peak was 27-88;
- reliable application backlog stayed at zero, and no rejected input, reliable drop, malformed,
  rejected, rate-limited, budget-dropped, or disconnected traffic occurred;
- every client moved at least 8.20 m, remained connected, and ended with zero position error.

## Scope limit

This is a deterministic, eight-client, in-process shared impairment workload. It does not measure
RFC IPDV, kernel scheduling, real sockets, fragmentation, correlated or burst loss, shared-link
rate/queue behavior, or congestion-control fairness. In particular, UDP supplies no inherent
congestion control; applications using it on the Internet must provide suitable behavior as
described by [RFC 8085](https://www.rfc-editor.org/rfc/rfc8085.html). This benchmark does not prove
that behavior.

It closes the maintained multi-client 100 ms RTT / 2% unreliable-loss prediction, convergence,
bandwidth, backlog, and server-P99 evidence slice. Socket-backed netem validation,
hostile/shared-link behavior, and workload-specific multi-hour impairment endurance remain separate
work. Timestamp-based process scaling is calibrated independently in the
[process temporal-aggregation benchmark](process_temporal_aggregation_benchmarks.md).
