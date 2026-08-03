# Process Architecture

Long-running transformations use a shared timestamp-based process model.

Implemented foundation:

- `ProcessDefinition`
  - stable process prototype id
  - default required work in authoritative world ticks
  - data-defined room requirement
  - data-defined power requirement and required capacity
  - base quality rate in per-mille
  - data tags for game/runtime specialization
  - materialized from process prototypes through `process_definition_from_prototype`

- `ProcessInstance`
  - stable process id
  - owner save id
  - prototype id
  - input and output slots with valid prototype ids and non-zero counts
  - start time and last update time
  - accumulated effective work
  - interruption state
  - output-claimed state, optional condition function id, and interruption policy
  - validates known process state, work/time consistency, complete-state work, and stale
    interruption reasons

- `ProcessIdAllocator`
  - reserves process ids independently from permanent `SaveId` values
  - owned by `WorldState` so server-authoritative commands do not reuse process ids

- `ProcessRuntime`
  - creates validated process instances
  - advances running processes from timestamps
  - applies room/power/quality modifiers as deterministic per-mille rates
  - supports interruption and resume without granting hidden offline progress
  - exposes typed lazy-evaluation triggers for chunk load, interaction, render proximity, state
    change, inspection, and save-load validation; the current foundation records the call boundary
    but all trigger kinds share the same timestamp advancement behavior
  - can create instances directly from validated `ProcessDefinition` records
  - participates in simulation LOD through process-owner subjects derived from spatial owner
    records, carrying both owner `SaveId` and process `ProcessId` without storing processes as
    entities or build pieces

- `ProcessEnvironmentResolver`
  - finds the room associated with a process owner from derived room source ids
  - converts room descriptors and available power capacity into shared process modifiers
  - reports readable factors and warnings such as missing room, missing power, and insufficient
    power
  - keeps `ProcessRuntime` generic while data-defined process requirements and game/runtime rules
    feed it settlement context from rooms and networks

- `ProcessTemporalAggregationController`
  - admits newly inserted process records under a hard per-tick count and total tracking cap
  - predicts one deterministic next event for every admitted non-complete process and orders equal
    due times by stable process id
  - resolves room, power, and prototype modifiers at admission and at the next event instead of
    scanning every process on every world tick
  - reevaluates interrupted or zero-rate processes at a bounded interval
  - caps dispatched events, per-event late catch-up, and aggregate catch-up on every owner-thread
    update
  - stages and validates a complete event batch before committing it, then returns exact process
    transitions plus admission, queue, lateness, catch-up, stale-event, and saturation telemetry
  - resets conservatively when a successful command may change rooms, spatial networks,
    assemblies, or explicitly advances every process, so changed environmental modifiers are
    resolved before another prediction is trusted

- `ServerRuntime` process execution
  - runs temporal process aggregation immediately after authoritative world-clock advancement and
    before entity finalization
  - shares the same authoritative room/power/prototype modifier resolver with process commands
  - emits automatic completion transitions as one authoritative report, one reliable event batch,
    and one typed world delta under the same host replication sequence
  - applies ordinary relevance and reliable-overload disconnect policy without manufacturing a
    command response for an automatic transition
  - publishes process temporal counters through tick reports, inspection, and Tracy plots

- `process.start`
  - server-authoritative command that validates a process prototype
  - rejects owners that do not reference an existing saved world object
  - materializes a `ProcessDefinition`
  - reserves a process id
  - creates and inserts a timestamped `ProcessInstance`

- `process.advance_all`
  - server-authoritative command for applying timestamp progress to all running processes
  - uses `WorldState::world_time`, not client time, wall time, the envelope timestamp, or frame
    count
  - rejects client-supplied process rate modifiers
  - materializes each process prototype definition to get room, power, and quality requirements
  - resolves per-process room and power modifiers from `WorldState` derived rooms and
    owner-scoped power-network ports
  - applies room, power, and quality modifiers through the shared per-mille rate model
  - commits only when at least one process changes state

The canonical units and prototype field are ticks and `default_required_work_ticks`. The C++ data
types and prototype loader retain the old `_ms` names as source/content compatibility aliases;
those aliases do not change the values into milliseconds. A save stores the same `u64` world-tick
domain used by `WorldState`.

This model is intentionally generic. Drying, firing, smoking, smelting, crop growth,
animal recovery, ward charging, and machine work should specialize it through game
runtime rules and mod prototypes rather than each inventing a private timer.

The deterministic
[process temporal-aggregation benchmark](../performance/process_temporal_aggregation_benchmarks.md)
compares this future-event path with a dense per-process scan, retains every logical tick, and gates
semantic parity, deterministic checksums, hard budgets, two-tick burst recovery/lateness, P99,
speedup, and modifier-resolution reduction.
