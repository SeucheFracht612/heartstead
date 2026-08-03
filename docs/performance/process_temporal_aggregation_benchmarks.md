# Process temporal-aggregation benchmarks

This benchmark is the process-simulation slice of the entity-city workload. It measures the
production future-event controller against a dense reference that evaluates every process on every
logical tick. Both paths receive the same deterministic corpus, world clock, modifier function, and
final-state checks. The comparison therefore answers whether timestamp-based process aggregation
reduces work without changing authoritative process outcomes.

It is not a stopwatch around a prediction helper. The temporal side uses a real `ProcessDatabase`
and `ProcessTemporalAggregationController`; the dense side advances an independent copy through
`ProcessRuntime::advance`. The dense reference deliberately scans its vector directly instead of
calling `ProcessDatabase::advance_all`, whose strong-rollback map copy would measure transaction
staging in addition to dense simulation.

## Default deterministic workload

Each process starts at world tick zero. The retained seed controls the due-time and rate mix:

- 65,536 process records;
- 600 logical ticks per pass;
- one unretained warmup pass and five retained repetitions;
- 2,048 processes due together at tick 300;
- 256 zero-rate processes reevaluated every 20 ticks;
- all other processes distributed deterministically over ticks 1 through 600;
- active rates of 1x, 2x, or 4x, represented as exact integral per-mille values;
- a 4,096-process admission budget and 1,024-event tick budget;
- 1,200 ticks of catch-up per event and 4,800 ticks across one controller update.

Required work is constructed from each active process's due tick and rate multiplier. A dense
one-tick scan and a predicted future event therefore produce exactly the same accrued work and final
state without floating-point tolerance. Zero-rate processes remain running with zero accrued work.

All temporal records are admitted at tick zero before retained logical timing begins. Admission
time and resolver calls remain visible per repetition, but admission does not contaminate normal
tick percentiles. Retained repetitions alternate whether the temporal or dense path runs first so
one path does not keep the same paired cache and thermal position.

The 2,048-process completion burst intentionally exceeds the 1,024-event budget. Ordinary due work
behind that burst makes the queue remain exhausted for two logical ticks, after which it must clear.
If a custom workload still has due events after its final measured tick, the harness may issue
same-time artifact-only drain passes. These rows have a non-zero `drain_pass`, perform no dense work,
are excluded from timing and logical-backlog distributions, and never advance world time beyond the
configured interval. They exist only so final semantic parity can still be inspected.

## Evidence and timing semantics

Schema v1 retains every normal logical tick and every artifact drain. A raw row includes:

- temporal and dense elapsed nanoseconds;
- resolver calls and admission, dispatch, evaluation, change, and completion counts;
- stale and retired event counts;
- active-event and unadmitted-process depth;
- evaluated, catch-up, processed-lateness, and oldest-deferred tick counts;
- event-budget, catch-up-budget, and counter-saturation flags.

The report also retains runtime version, commit and dirty state, build type, compiler, platform,
operating system, CPU model, logical CPU count, seed, every controller limit, and every gate. Pooled
logical-tick timing reports minimum, median, P95, P99, maximum, mean, standard deviation, and
coefficient of variation. Every retained repetition has the same independent summary. The reported
speedup is the median of the five per-repetition dense/temporal median ratios, rather than a ratio
from one favored pass.

Final parity compares process identity, owner, prototype, required and accrued work, state, and
output-claim state. A bounded event processed after its predicted due tick may legitimately retain a
later `last_eval` than the dense scan, so timestamp differences are counted separately rather than
hidden inside the semantic checksum. Processed event lateness has its own hard two-tick gate. The
default burst consequently reports timestamp differences while still requiring identical semantic
checksums, final work, and states.

Default acceptance is:

| Measure | Gate |
| --- | ---: |
| Dense-reference semantic parity mismatches | 0 |
| Cross-path or cross-repetition checksum mismatches | 0 |
| Unexpected outcomes, stale events, or retired events | 0 |
| Hard admission/event/catch-up/counter violations | 0 |
| Continuous due-event backlog | at most 2 logical ticks |
| Processed event lateness | at most 2 ticks |
| Temporal logical-tick P99 | at most 5.0 ms |
| Median dense/temporal speedup | at least 5.0x |
| Modifier-resolver call reduction | at least 95% |

The 5 ms absolute limit uses the research starting envelope for complete game/simulation CPU work on
the minimum client as an outer fail-safe ceiling. It does not reserve that complete envelope for
processes. The 5x paired relative gate has more than threefold headroom against the clean reference
calibration below. These are regression rails, not portable hardware guarantees. Setting the P99,
speedup, or resolver-reduction limit to zero disables only that quantitative gate; correctness,
deterministic state, budget, backlog, and lateness gates always remain active.

## Commands and exit behavior

Build and run the optimized benchmark:

```bash
cmake --preset default-release
cmake --build build/default-release \
  --target heartstead_process_temporal_aggregation_benchmark -j2

build/default-release/apps/process_temporal_aggregation_benchmark/\
heartstead_process_temporal_aggregation_benchmark \
  --output build/default-release/benchmarks/process-temporal-aggregation.json
```

The executable rejects an unoptimized build. It emits the complete JSON before evaluating the
process exit status: zero means every gate passed, one is an execution or artifact-write failure,
two is an invalid command line or unoptimized build, and three means the report is valid but at
least one gate failed. This preserves failure evidence for CI. `--help` lists workload, budget,
seed, repetition, and gate overrides.

Run the small two-repetition contract test with:

```bash
cmake --build build/default-debug-werror \
  --target heartstead_process_temporal_aggregation_benchmark_tests -j2
ctest --test-dir build/default-debug-werror --output-on-failure \
  -R '^heartstead_process_temporal_aggregation_benchmark_tests$'
```

Raw reports belong under the ignored build tree, not in Git. Performance comparisons should use
optimized, thermally stable, otherwise idle machines; retain independent process artifacts with
matching provenance. The warmup/repetition/raw-output policy follows the official
[Google Benchmark user guide](https://github.com/google/benchmark/blob/main/docs/user_guide.md),
although Heartstead owns this deterministic domain harness and does not depend on that library.

## Clean reference calibration

On 2026-08-02, three independent Release processes from clean commit
`25466f203f938006ae03005ee2e5cdde580f0bbf` passed all nine gates on an Intel Core Ultra 7 258V
with GCC 13.3.0 on Linux 6.17.0-1030-oem. Each process retained the default seed, one warmup pass,
five measured repetitions, and 3,000 ordinary raw tick rows; every report recorded
`git_dirty=false`.

| Process | Temporal median | Temporal P95 | Temporal P99/max | Dense median | Median speedup |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.070609 ms | 0.109926 ms | 0.140991/0.294165 ms | 1.265483 ms | 17.952951x |
| 2 | 0.070482 ms | 0.108932 ms | 0.139361/0.311880 ms | 1.248409 ms | 17.725861x |
| 3 | 0.072293 ms | 0.112510 ms | 0.146213/0.318098 ms | 1.276090 ms | 17.723545x |
| **Median** | **0.070609 ms** | **0.109926 ms** | **0.140991/0.311880 ms** | **1.265483 ms** | **17.725861x** |

Every retained repetition made 138,240 temporal resolver calls versus 39,321,600 dense calls, a
99.6484% reduction. The maximum continuous backlog and processed lateness were exactly two ticks.
There were zero semantic parity mismatches, checksum mismatches, unexpected outcomes, and hard
budget violations. All paths and repetitions ended at checksum `4895635283162560062`. Each
repetition reported 1,498 timestamp differences caused by bounded late processing; those
differences remained within the explicit two-tick lateness gate and did not change work or final
state.

## Scope limits

This is a headless CPU process benchmark, not a complete game/server entity-city macrobenchmark. It
does not measure entity AI, spatial queries, physics, rendering, network impairment, save work,
allocator ownership, or total server tick time. The modifier resolver is deterministic and cheap;
production room, power, and prototype resolution has separate runtime coverage. Mid-run dependency
changes force conservative controller reset and re-admission in `ServerRuntime`, but this static
corpus does not price repeated resets.

The controller currently aggregates the generic timestamp-based production model. Crop, animal,
population, economy, and settlement aggregate models still need their own deterministic state
contracts and scale workloads. Historical regression decisions must compare machine-attributed raw
artifacts; the paired dense reference is an architectural baseline, not a substitute for a clean
previous-commit measurement. Multi-hour endurance remains separate work.
