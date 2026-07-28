# Particles

Particles are bounded presentation state. Gameplay and mods address a particle prototype, never a
renderer asset path. The base mod currently declares fire embers, block-break puffs, and water
splashes as `particle` prototypes. Content validation materializes those generic records into typed
`ParticlePrototype` values and rejects invalid lifetimes, speed/size ranges, colors, material
groups, and atlas layouts before a session starts.

`CpuParticleSystem` owns a pre-reserved dense pool, a bounded event queue, and generation-safe
retained emitters. Burst events and lifetime/rate emitters share the same spawn path. Each request
has an explicit seed, and the system processes events, emitters, particles, and removals in stable
order. The overflow policy is drop-new: the pool never reallocates past its configured maximum,
and dropped events and particles remain visible in telemetry. Particle serials are presentation
identities only; particles do not enter saves or authoritative replication.

Simulation keeps exact `WorldPosition` anchors and updates only local offsets, so effects remain
stable at large world coordinates and work with the renderer's floating origin. Spawned particles
cache their gravity and drag policy, avoiding a prototype lookup in the hot update loop. Dense
in-place compaction avoids per-frame allocations. Previous/current positions support render
interpolation, while size, color fade, roll, and atlas frame are derived from normalized age.

`ParticlePresentation` uploads one shared quad and retains one render object per accepted particle.
Each quad is camera-facing, transparent, two-sided, and grouped by the existing
mesh/material/layer instance batching path. The atlas frame occupies the fourth instance metadata
word; the current colored-particle material does not sample an authored texture yet, but the
instance ABI already carries the frame without requiring another scene-object format change.
Presentation capacity is independently bounded and reports drops. Render-object child counts make
unparented particle removal constant-time even when tens of thousands expire together.

`RendererStats` and renderer benchmark JSON expose CPU update time, presentation synchronization
time, active particles/emitters, material groups, and drops. The `particles` benchmark scene holds
50,000 active billboards in front of the camera. The Release reference run on the development
machine (10 measured frames after 3 warm-up frames) recorded:

- 0.495 ms median / 0.655 ms p95 particle simulation update;
- 4.086 ms median / 4.234 ms p95 retained presentation synchronization;
- 50,000 submitted instances in one particle draw, one material group, zero drops.

Run the same workload with:

```sh
build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene particles --radius 0 --warmup 60 --frames 300 \
  --output build/default-release/particle-benchmark.json
```

`dev_game` creates a retained ember emitter beside the spawn area and queues deterministic bursts
when a block is removed or the local player enters swimming. These are visual responses; any future
particle that affects gameplay must originate from an authoritative gameplay event while still
remaining simulated only by presentation.
