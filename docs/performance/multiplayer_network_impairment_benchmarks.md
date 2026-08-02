# Multiplayer network-impairment benchmarks

This benchmark measures the maintained prediction and replication path under deterministic network
impairment. It creates a real headless `GameRuntime`, authoritative server, and local client over
the production in-memory transport. After a bounded stable bootstrap, the client submits one
forward movement input on each of 600 measured 60 Hz ticks while the runtime exercises input-bundle
redundancy, unreliable loss, prediction, authoritative movement, transient replication,
reconciliation, chunk interest, transport maintenance, and client synchronization.

The default stimulus is:

- 50 ms configured one-way latency, or 100 ms nominal RTT;
- uniform configured delay variation of plus or minus 10 ms;
- 2% loss applied only to unreliable messages;
- a retained 64-bit seed for the impairment stream;
- four consecutive stable warmup ticks after connection, local-player publication, zero reliable
  application backlog, no partial chunk snapshot, and at most 32 in-flight impaired messages.

Each measured input uses a unique sequence. The benchmark fails immediately if a frame does not
produce exactly one authoritative tick, client synchronization reports an error, required player
state disappears, or the final report cannot reproduce its summary from the raw samples.

## Measurement terminology

[RFC 3393](https://www.rfc-editor.org/rfc/rfc3393.html) defines IP packet delay variation from the
difference between selected one-way-delay samples and notes that “jitter” is used ambiguously. This
in-process workload does not timestamp packet arrivals as an IPPM measurement, so the report calls
the configured uniform plus-or-minus parameter `simulated_delay_variation_ms`; it does not claim to
measure RFC IPDV.

[RFC 7680](https://www.rfc-editor.org/rfc/rfc7680.html) defines a sample loss ratio from losses over
the sampled packet population. The transport therefore reports an explicit
`impairment_eligible_unreliable_message_count` beside simulated drops. The benchmark's observed
loss ratio is drops divided by that eligible population, never by a mixed reliable/unreliable
message count. Linux [`tc-netem(8)`](https://man7.org/linux/man-pages/man8/tc-netem.8.html) likewise
separates delay variation, loss, and a reproducibility seed; Heartstead's simulator is its own
uniform, deterministic implementation and does not claim netem's distributions or kernel timing.

Wire-byte counters are encoded offered load. The in-memory transport charges a message before its
simulated unreliable-loss decision, so dropped traffic remains visible in bandwidth demand. This
is deliberately stricter than counting only delivered bytes.

## Raw evidence and gates

Schema-v1 JSON retains source/build/CPU provenance, exact stimulus and gate configuration, a
summary, all gate violations, and every measured tick. Raw rows include:

- server-tick wall time, complete runtime-frame wall time, and scheduler time;
- accepted, rejected, repeated, predicted, reconciled, and acknowledged input work;
- per-frame hard corrections and maximum correction distance;
- encoded bytes and messages in each direction;
- loss-eligible unreliable messages, simulated drops, and current in-flight impairment depth;
- reliable application backlog messages/bytes;
- reliable drops, malformed/rejected/rate-limited traffic, outbound-budget drops, disconnections,
  and current connection state.

The summary adds nearest-rank P50/P95/P99/max timing, correction-distance percentiles, a rolling
60-tick server-to-client byte maximum, observed eligible-unreliable loss ratio, peak/final backlog,
authoritative displacement, and the final acknowledged sequence. The default evaluated gates are:

| Measure | Gate |
| --- | ---: |
| Server tick P95 | at most 12.5 ms |
| Server tick P99 | at most 16.667 ms |
| Maximum server tick | at most 50 ms |
| Accepted unique inputs | more than 90% of measured ticks |
| Hard corrections | at most 1 |
| Maximum correction distance | less than 1 m |
| Average encoded server-to-client offered load | less than 64 KiB/s |
| Rolling one-second encoded server-to-client offered load | less than 256 KiB |
| Loss-eligible unreliable messages / simulated drops | at least 1 / at least 1 |
| In-flight impaired messages | at most 128 |
| Authoritative displacement | more than 5 m |
| Reliable application backlog at final measured boundary | zero messages and bytes |
| Reliable drops, malformed/rejected/rate-limited traffic, budget drops, disconnections | zero |
| Final client state | connected |

The impairment queue is not required to become zero while the measured server continuously emits
movement snapshots. Its raw final value and peak are retained and the peak is bounded instead. The
separate reliable application FIFO must be empty at the final measured boundary.

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

Run the smaller 120-tick contract test:

```bash
cmake --build build/default-debug-werror \
  --target heartstead_multiplayer_network_impairment_benchmark_tests -j2
ctest --test-dir build/default-debug-werror --output-on-failure \
  -R '^heartstead_multiplayer_network_impairment_benchmark_tests$'
```

Use `--help` for stimulus and gate overrides. Raw reports belong under the ignored build tree, not
in Git. Compare independent optimized processes with the same commit, build configuration, seed,
content, and machine.

## Clean reference calibration

On 2026-08-02, three independent Release processes from clean commit
`be4c60db7bc14b305de302c1eff6b19e4dfd46db` passed every gate and report invariant on an Intel Core
Ultra 7 258V with GCC 13.3.0 on Linux 6.17.0-1030-oem. Every report retained
`git_dirty=false`, the default seed, an 11-tick warmup, and 600 measured ticks.

| Process | Server P50 | Server P95 | Server P99/max | Runtime-frame P99/max | Peak impaired |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.015 ms | 0.018 ms | 0.019/0.657 ms | 0.134/0.659 ms | 70 |
| 2 | 0.016 ms | 0.018 ms | 0.020/0.684 ms | 0.141/0.685 ms | 69 |
| 3 | 0.015 ms | 0.018 ms | 0.019/0.682 ms | 0.130/0.684 ms | 69 |
| **Median** | **0.015 ms** | **0.018 ms** | **0.019/0.682 ms** | **0.134/0.684 ms** | **69** |

Behavioral and traffic evidence was identical across the three processes:

- 598 of 600 inputs were accepted inside the measured interval (99.667%), two ticks repeated the
  previous input, all 600 inputs were predicted, and the final acknowledged sequence was 600;
- there were zero hard corrections and the maximum soft correction distance was 0.075 m;
- 1,197 unreliable messages were loss-eligible and 19 were dropped, an observed sample ratio of
  1.587%;
- encoded server-to-client offered load averaged 22,253.6 bytes/s and peaked at 34,103 bytes in a
  rolling one-second window;
- the final impairment depth was five, the reliable application FIFO ended at zero, and no
  transport error, budget drop, or disconnect occurred;
- authoritative movement covered 23.2 m and the client remained connected.

## Scope limit

This is a deterministic, single-client, in-process runtime workload. It does not measure RFC IPDV,
kernel scheduling, real sockets, fragmentation, retransmission, correlated or burst loss,
congestion control, hostile traffic, multiple simultaneous impaired clients, or whole-process
long-soak memory slope. It closes the maintained 100 ms RTT / 2% unreliable-loss prediction,
bandwidth, backlog, and server-P99 evidence slice. Multi-client impairment, long-soak, and
game-specific temporal aggregation remain M6 work.
