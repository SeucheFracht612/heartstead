# Predictive streaming benchmarks

This benchmark measures whether bounded speculation improves traversal without hiding cache
pollution, cancellation loss, owner-thread work, or residency growth. It runs two isolated worlds
with the same seed and path:

1. a baseline with velocity and camera lookahead disabled;
2. the production predictive policy and controller.

Both trials use the deterministic terrain generator, `ChunkLoadScheduler`, narrow owner-thread
publication, real cancellation results, `WorldState` insertion, and actual clean-chunk eviction.
The only paired variable is predictive admission. One base chunk is generated before timing so the
comparison begins from an identical resident position rather than startup work.

## Workload

The default path retains every movement step and includes:

- 20 one-chunk steady-forward steps;
- 8 immediate reversal steps;
- one 256-chunk two-axis teleport under critical pressure;
- 6 post-teleport recovery steps, with the first two under elevated pressure;
- 32 nominal-pressure soak steps used for the residency-slope calculation.

A bounded cancellation probe changes prediction direction for one owner update immediately before
the reversal. This makes a real in-flight cancellation observable even on a machine that can finish
one generated chunk inside the 20 ms movement interval. It does not add a movement sample or sleep
between submission and invalidation.

The controller always processes demand first. If any immediate request is deferred, it admits no
new speculation. Otherwise it keeps at least `reserved_required_request_slots` free, submits
speculation at low priority, cancels work outside current immediate/predictive interest, applies
ranked clean evictions in `max_evictions_per_update` waves, and reports every scheduler outcome back
to the predictor. The tail drain continues until deferred eviction work is reconciled.

## Metrics and fail-closed invariants

The schema-v1 JSON retains one row per movement step for both trials, including phase, coordinate,
pressure, immediate residency, visible-hole exposure, deadline residency, resident chunks, pending
loads, active speculation, and cumulative eviction.

The predictive policy separately reports:

- submitted and published speculation;
- useful, timely, late, and wasted predictions;
- accuracy, coverage, and prefetch-to-use lead time;
- requested and completed cancellation, plus cancellation races;
- failed and stale speculative results;
- bounded eviction output, deferred evictions, projected post-wave overage, and overage that clean
  candidates cannot resolve.

The report is rejected if either trial loses a movement row, retains an active request or memory
reservation, sees failed/stale/rejected/duplicate scheduler work, fails to account for every
submission as publication or cancellation, exceeds its critical final residency cap, or fails to
distinguish the baseline from predictive behavior.

Starting gates are:

| Measure | Gate |
| --- | ---: |
| Predictive visible-hole P95 | at most 250 ms |
| Predictive immediate-hit rate | at least 0.50 |
| Resolved prediction accuracy | at least 0.25 |
| Resolved prediction waste ratio | at most 0.75 |
| Completed/requested cancellation ratio | at least 0.75 |
| Predictive/baseline visible-hole rate | at most 1.0 |
| Late-soak residency slope | at most 0.05 chunks/step |
| Worst owner publication update | at most 500 us |

These are initial engineering gates. The JSON records the exact configured limits, scheduler and
policy caps, runtime metadata, commit, dirty state, compiler, OS, and CPU.

## Commands

Build and run the optimized benchmark:

```bash
cmake --build build/default-release \
  --target heartstead_predictive_streaming_benchmark -j2

build/default-release/apps/predictive_streaming_benchmark/heartstead_predictive_streaming_benchmark \
  --enforce-gates \
  --output build/default-release/benchmarks/predictive-streaming.json
```

Run the small paired contract test:

```bash
cmake --build build/default-release \
  --target heartstead_predictive_streaming_benchmark_tests -j2
build/default-release/tests/heartstead_predictive_streaming_benchmark_tests
```

Large/raw reports stay outside Git. Calibrations must use a clean tracked tree and record independent
processes rather than treating repeated trials in one process as independent machines.

## Clean reference calibration

Three independent Release processes from clean commit
`e305891e360605464474b4847ca8f08e030534bd` passed every gate and fail-closed invariant on an
Intel Core Ultra 7 258V with GCC 13.3.0 on Linux 6.17.0-1030-oem. Each process ran an isolated
baseline trial followed by an isolated predictive trial; the reports recorded `git_dirty=false`.

| Process | Baseline holes | Predictive holes | Predictive immediate hit | Accuracy | Waste | Cancellation | Soak slope |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 59/67 | 30/67 | 55.22% | 82.61% | 12 | 4/4 | 0.00 chunks/step |
| 2 | 59/67 | 30/67 | 55.22% | 82.61% | 12 | 4/4 | 0.00 chunks/step |
| 3 | 59/67 | 30/67 | 55.22% | 82.61% | 12 | 4/4 | 0.00 chunks/step |

| Process | Baseline hole P95 | Predictive hole P95 | Worst owner publication | Maximum/final predictive residency |
| ---: | ---: | ---: | ---: | ---: |
| 1 | 5.696 ms | 5.658 ms | 28 us | 16/3 chunks |
| 2 | 6.535 ms | 5.799 ms | 46 us | 16/3 chunks |
| 3 | 6.552 ms | 5.614 ms | 102 us | 16/3 chunks |
| **Median** | **6.535 ms** | **5.658 ms** | **46 us** | **16/3 chunks** |

Across these processes the predictive policy reduced visible-hole steps by 49.2% relative to the
baseline and raised immediate residency from 11.94% to 55.22%. Timely coverage was 85.07% in every
run. All submissions were accounted for as publication or cancellation, no trial ended with pending
work or reservations, no failed/stale/rejected/duplicate work occurred, and no late-soak residency
growth was observed. These values calibrate the deterministic headless workload below; they are not
client display, network, or whole-process memory results.

## Scope limit

This is a headless generation/residency benchmark. It does not yet measure saved-delta I/O, client
camera integration, mesh/upload/display completion, multiplayer spread, byte-based whole-process
memory pressure, or network replication. It validates comparative policy/controller behavior; the
live renderer-proof integration separately covers authoritative motion, teleport, bounded eviction,
and exact local-client replacement. This benchmark does not by itself close all of M6.
