# Simulation LOD Architecture

Long-lived settlements need different simulation detail depending on player proximity
and load state.

Implemented foundation:

- `SimulationSubject`
  - stable save id for persistent objects
  - optional runtime handle for currently materialized entities
  - stable process id for process-owner subjects
  - prototype id
  - subject kind
  - world coordinate
  - last update timestamp
  - positive estimated update-work units for deterministic admission control
  - sleeping and forced-LOD state

- `SimulationViewer`
  - network id
  - world coordinate

- `SimulationLodPolicy`
  - full simulation radius
  - simplified simulation radius
  - tick intervals for full, simplified, and sleeping subjects

- `SimulationLodPlanner`
  - classifies subjects as `full`, `simplified`, `sleeping`, or `unloaded`
  - reports due ticks for loaded subjects
  - reports offline deltas for unloaded subjects so timestamp-based systems can advance
    when reloaded
  - validates radius, timestamp, and saved-identity invariants

- `SimulationTickBudget` and `plan_budgeted_frame`
  - cap admitted subject count and estimated work units on every authoritative tick
  - order due work by oldest deadline, then LOD importance and stable subject identity, so input
    container order cannot choose winners and persistently deferred work becomes oldest first
  - reject duplicate stable identities and subjects whose individual estimated cost can never fit
    the complete tick budget instead of starving them forever
  - cap analytical catch-up both per update and across the tick
  - keep full-detail catch-up at zero by default, so a delayed full-detail subject receives its
    normal fixed interval rather than an unsafe large time step
  - expose the admitted update delta, analytical catch-up portion, remaining elapsed time, and exact
    next-update timestamp for each scheduled subject
  - retain scheduled/deferred counts, work units, simulated time, catch-up debt, maximum lateness,
    saturation, and budget-exhaustion telemetry

- `derive_simulation_subjects`
  - world-layer adapter that derives deterministic simulation subjects from `WorldState`
  - emits entity subjects from `EntityRecord` transforms and build-piece subjects from
    `BuildPieceRecord` transforms
  - emits assembly subjects from assembly root build-piece transforms and uses assembly operating
    state for sleeping classification
  - emits process-owner subjects for process instances whose owners currently have spatial
    transforms through build pieces, persistent entities, cargo records, or assembly roots
  - keeps the process owner's `SaveId` as the spatial owner while carrying the process instance's
    `ProcessId` so multiple processes on one object remain distinguishable in frame plans
  - emits non-persistent network subjects from spatial network nodes using deterministic runtime
    handles, so derived networks do not claim saved identity
  - emits non-persistent chunk-region subjects from terrain chunk coordinates using deterministic
    runtime handles, so terrain chunks do not become saved objects or entities
  - keeps the generic LOD planner independent from world database ownership
  - supports filtering subject kinds without merging entity, build-piece, assembly, process,
    network, or chunk storage

- `plan_world_simulation_frame`
  - derives world simulation subjects and classifies them through the generic planner in one
    world-layer call
  - keeps game/runtime callers from needing to know how entities, build pieces, assemblies,
    process owners, networks, and chunks map into generic subjects
  - propagates subject-derivation and planner validation errors without mutating `WorldState`

- `plan_budgeted_world_simulation_frame`
  - applies the same temporal budget to deterministic subjects derived from `WorldState`
  - preserves the raw classification plan alongside the ordered admitted-update list
  - remains side-effect free so a failed game-specific update cannot advance its timestamp

- `derive_replication_relevance_policy`
  - reuses derived simulation subjects and viewer positions to build per-client replication
    interest rules
  - includes full, simplified, and sleeping saved subjects by default while leaving unloaded
    subjects out of normal replication relevance
  - has an inspectable report form for subject totals, per-viewer visible saved subjects, LOD
    exclusions, and non-saved subject skips
  - has a world-layer helper that installs the derived policy on a `HostSession` and returns the
    report for tooling
  - keeps the generic networking relevance policy free of world database knowledge

- debug inspection
  - exposes policy radii and tick intervals
  - exposes tick admission and per-LOD catch-up limits
  - exposes raw subject identity, coordinates, timestamps, persistence, sleeping, and forced-LOD
    state before frame planning
  - exposes per-subject LOD decisions, process ids, due-tick state, work estimate, and offline delta
  - exposes frame-plan counts and reports inconsistent decision/count summaries as errors
  - exposes budgeted admission, backlog, catch-up, saturation, and maximum-lateness counters and
    reports a plan that contradicts its hard limits or scheduled updates as invalid
  - exposes world-derived replication interest reports before host sessions consume the
    resulting network relevance policy

The planner is intentionally generic. Game runtime systems should decide what a full,
simplified, or reload-time update means for animals, crops, machines, wards, storage,
outposts, and cargo. The engine owns the shared classification and timing contract.

## Catch-up commit contract

A consumer applies scheduled updates in the returned order. It advances a subject timestamp to
`next_update_time_ms` only after that subject's update succeeds. It must not set the timestamp to the
frame's `now_ms` when `remaining_delta_ms` is non-zero: doing so would erase simulation debt that was
not admitted. A failure leaves the prior timestamp intact and can therefore be retried.

Full-detail work receives one fixed interval per admitted update by default. Simplified and sleeping
models may opt into larger analytical deltas, but only within their per-update and aggregate tick
limits. If an update leaves at least one complete interval behind, the plan reports remaining
catch-up work and `budget_exhausted=true`. A sub-interval remainder is normal cadence, not backlog.
The configured cadence cannot become faster as detail decreases.

A subject returning after a long unloaded interval should run its deterministic aggregate/reload
model before promotion to full-detail behavior. Otherwise a deliberately fixed-step full-detail
model may need many bounded ticks to retire old debt, which is observable but not a useful reload
strategy.

Estimated work units make selection reproducible and bound per-item overhead; they do not prove a
wall-clock tick budget. Server benchmarks must calibrate estimates against measured system and tick
durations, and runtime systems must keep aggregate catch-up deterministic for their own state model.

The variable-frequency, per-LOD grouping parallels
[Unreal Mass Simulation LOD](https://dev.epicgames.com/documentation/en-us/unreal-engine/overview-of-mass-gameplay-in-unreal-engine).
Its option to spread first updates across an LOD period also reinforces avoiding synchronized
activation bursts; Heartstead currently contains such bursts through deterministic admission and
reports the remaining backlog rather than claiming they disappeared.
Oldest-deadline ordering is informed by Liu and Layland's original
[deadline-driven scheduling analysis](https://www.cis.upenn.edu/~lee/07cis505/Lec/liu73scheduling.pdf),
but this cooperative game scheduler does not claim hard-real-time guarantees. Fixed full-detail
steps and bounded catch-up follow the overload concern described in
[Fix Your Timestep](https://gafferongames.com/post/fix_your_timestep/): admitting unbounded recovery
work can make an already-late simulation fall farther behind.
