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
ranked clean evictions, and reports every scheduler outcome back to the predictor.

## Metrics and fail-closed invariants

The schema-v1 JSON retains one row per movement step for both trials, including phase, coordinate,
pressure, immediate residency, visible-hole exposure, deadline residency, resident chunks, pending
loads, active speculation, and cumulative eviction.

The predictive policy separately reports:

- submitted and published speculation;
- useful, timely, late, and wasted predictions;
- accuracy, coverage, and prefetch-to-use lead time;
- requested and completed cancellation, plus cancellation races;
- failed and stale speculative results.

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

## Scope limit

This is a headless generation/residency benchmark. It does not yet measure saved-delta I/O, client
camera integration, mesh/upload/display completion, multiplayer spread, byte-based whole-process
memory pressure, or network replication. It validates the shared policy/controller boundary before
those systems adopt it; it does not by itself close M6.
