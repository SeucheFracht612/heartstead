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
  - validates shape, mass, position, velocity, gravity, and timestep inputs
  - steps the world through an explicit timestep
  - supports broad-phase AABB overlap queries
  - drains per-step contact records for debug, gameplay, and tests

- Shape descriptors
  - box
  - sphere
  - capsule
  - compound shapes with child shapes

- `PhysicalResourceRecord`
  - uses one compound physics body for felled trees and other large physical resources
  - stores stable resource identity separately from runtime physics body ids
  - moves from cutting, dynamic, settled, frozen, and cargo-converted lifecycle states

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
  - constrains bodies to translational degrees of freedom until the public boundary grows rotation
    and angular-velocity state
  - uses zero damping/friction/restitution at this foundation layer so backend comparison exercises
    the same policy as the headless reference
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

This layer is deliberately separate from `engine/entities/`, `engine/world/`, and
`engine/save/`. A saved entity, build piece, cargo object, or felled tree can reference
its own stable save id while recreating physics bodies when loaded. Save files must not
store raw physics ids or backend handles as permanent identity.

The headless backend remains the deterministic reference for runtime body ownership, validation,
stepping, compound-body representation, broad-phase queries, contact plumbing, dynamic-body
correction, and sleeping state. `physics_sandbox` repeats the same settling scenario through both
backends and fails if either backend is internally nondeterministic or if their final position,
velocity, contact, and sleeping results exceed the published tolerances.

Jolt `CharacterVirtual` and authoritative physical-resource body synchronization are the next M1
slices. Their detailed contracts and acceptance budgets live in
`docs/roadmap/gameplay_foundations_m1_m8.md`.
