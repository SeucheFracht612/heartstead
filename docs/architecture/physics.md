# Physics Architecture

Physics is an engine-owned integration boundary. Gameplay objects may create and own
physics bodies through engine APIs, but physics body ids are runtime ids, not permanent
save identity and not entity ids.

Implemented foundation:

- `PhysicsBackend`
  - names backend slots: `headless` and `jolt`
  - lets tests, tools, and samples query backend availability

- `PhysicsBackendCapabilities`
  - reports backend availability before world creation
  - describes deterministic stepping, dynamic/kinematic/static bodies, compound shapes,
    AABB queries, contacts, sleeping, character controllers, constraints, collision response,
    and backing library
  - lets gameplay and tooling understand the Jolt contract without owning Jolt handles

- `IPhysicsWorld`
  - creates and destroys runtime physics bodies
  - exposes body state through `PhysicsBodyId`
  - exposes backend capability metadata from the created world
  - validates shape, mass, position, linear/angular velocity, gravity, and timestep inputs
  - steps the world through an explicit timestep
  - supports broad-phase AABB overlap queries
  - drains per-step contact records for debug, gameplay, and tests
  - accepts engine-owned body rotations while keeping Jolt quaternions private
  - creates backend-owned virtual character controllers when the backend advertises support

- Shape descriptors
  - box
  - sphere
  - capsule
  - compound shapes with child shapes

- `PhysicalResourceRecord`
  - uses one compound physics body for felled trees and other large physical resources
  - stores stable resource identity separately from runtime physics body ids
  - moves from cutting, dynamic, settled, frozen, and cargo-converted lifecycle states
  - persists authoritative position, rotation, and linear/angular velocity while rebuilding
    runtime-only body ids after load

- `PhysicalResourcePhysicsSystem`
  - creates and restores bodies in stable save-id order
  - synchronizes live body transforms and velocities back into authoritative world records
  - observes backend sleeping, transitions dynamic resources to settled, and replaces long-sleeping
    bodies with static bodies after a configurable tick threshold
  - owns body cleanup and exposes active/dynamic/sleeping/frozen plus per-tick lifecycle counters
  - runs once before and once after the authoritative physics phase

- Motion types
  - static bodies for terrain/building collision
  - kinematic bodies for controlled movement
  - dynamic bodies for simulated objects

- Headless backend
  - compiles everywhere
  - integrates dynamic and kinematic bodies deterministically
  - supports impulses for dynamic bodies
  - computes conservative shape AABBs for boxes, spheres, capsules, and compounds
  - reports overlapping body pairs as contact records
  - applies deterministic AABB positional correction for dynamic contacts
  - removes velocity into the contact normal for simple non-bouncy collision response
  - sleeps settled dynamic bodies after repeated low-velocity contact steps

- Jolt backend
  - is provided by the pinned `joltphysics` vcpkg dependency and can be disabled explicitly with
    `HEARTSTEAD_ENABLE_JOLT=OFF`
  - owns Jolt allocator/type registration, `PhysicsSystem`, layer filters, temporary allocator,
    bounded job system, body-id map, and contact listener without exposing Jolt types publicly
  - implements box, sphere, capsule, and static-compound shapes plus static, kinematic, and dynamic
    bodies
  - implements fixed stepping, configured mass/gravity, sleeping, impulses, broad-phase AABB
    queries, and deterministic engine-owned contact snapshots
  - gives dynamic bodies all rotational and translational degrees of freedom and returns live
    quaternion state as engine-owned Euler rotation plus angular velocity
  - uses non-bouncy contact, moderate friction, and bounded linear/angular damping so dropped
    resources converge to Jolt sleeping instead of sliding or spinning forever
  - reports `physics.jolt_unavailable` only in builds that explicitly disable the dependency

- Voxel terrain collision
  - snapshots one immutable chunk plus a revisioned prototype collision table
  - greedily merges full-cube voxels while preserving exact partial prototype collision bounds
  - treats prototypes with empty collision bounds, including fluids and decorative occupancy, as
    non-colliding
  - cooks on a bounded worker queue and applies at most two results within a `2 ms` owner-thread
    budget per simulation tick
  - rejects results from stale chunk load generations, content revisions, or palette revisions
  - creates one chunk-local static compound body and removes it when the chunk unloads
  - consumes only collision dirty regions, leaving mesh and lighting work for their own schedulers
  - exposes pending, in-flight, stale, box-count, cook-time, and apply-time statistics through the
    runtime inspection path

- Character movement
  - `ICharacterCollisionWorld` keeps the Souls-style player state machine independent of voxel and
    Jolt implementations
  - `FixedStepPlayerInputScheduler` retains render-frame key edges and mouse deltas, then emits
    exactly one input per simulation step, so prediction speed and server queue depth do not depend
    on monitor refresh rate
  - prediction reconciliation removes acknowledged sequences and replays the remainder against the
    already-replicated client voxel world; collision-world revision changes do not discard pending
    movement/look state, and correction distance is measured only after replay between equivalent
    predicted states
  - the deterministic voxel implementation remains the headless and prediction reference
  - `PhysicsCharacterCollisionWorld` owns one backend virtual character per authoritative player
    and converts exact world positions through the same bounded physics-island frame as terrain
  - Jolt uses a foot-anchored capsule, a configured 50-degree slope limit, `ExtendedUpdate`
    stair-walking/floor-stick behavior, penetration recovery, stance resize checks, and dynamic-body
    impulses
  - cooked voxel chunks are Jolt compound bodies; when `CharacterVirtual` stalls at a convex
    floor/riser subshape seam, the exact voxel solver may propose a terrain step, but the candidate
    is accepted only after Jolt validates the complete character shape at that position; a tight
    voxel-support probe suppresses transient false-airborne states without creating ledge hover
  - ladder, fluid, unloaded-chunk, and ledge queries deliberately remain voxel-semantic queries;
    swimming and physical-resource buoyancy use the authoritative fluid-volume query while Jolt
    retains terrain and build collision ownership
  - the third-person camera sweeps a radius against each voxel prototype's declared collision
    bounds, retracts immediately when the safe boom distance shrinks, and restores outward at a
    time-based speed after space clears; unloaded chunks remain conservative solid occluders
  - each controller tick returns non-persistent diagnostics for requested, applied, and
    depenetration displacement plus axis hits, ceiling contact, step-up, and unloaded-chunk
    blocking; the local prediction client retains the latest tick for the development overlay
  - `dev_game` selects Jolt for interactive sessions while deterministic headless smoke runs and
    dedicated-server fixtures retain the reference backend

This layer is deliberately separate from `engine/entities/`, `engine/world/`, and
`engine/save/`. A saved entity, build piece, cargo object, or felled tree can reference
its own stable save id while recreating physics bodies when loaded. Save files must not
store raw physics ids or backend handles as permanent identity.

The headless backend remains the deterministic reference for runtime body ownership, validation,
stepping, compound-body representation, broad-phase queries, contact plumbing, dynamic-body
correction, and sleeping state. `physics_sandbox` repeats the same settling scenario through both
backends and fails if either backend is internally nondeterministic or if their final position,
velocity, contact, and sleeping results exceed the published tolerances.

Runtime verification belongs in [testing](../dev/testing.md). Current executable coverage and
known limits live in [project status](../project_status.md).
