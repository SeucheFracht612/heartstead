# Particles and transient effects

Particles are bounded presentation state. Gameplay and mods address a particle prototype, never a
renderer asset path. Content validation materializes generic prototype records into typed
`ParticlePrototype` values and rejects invalid lifetimes, speed/size ranges, colors, atlas layouts,
blend/shading/geometry modes, collision settings, LODs, and budgets before a session starts.

The base catalog covers dust, fragments, leaves, sparks, embers, smoke, steam, rain, snow, spores,
pollen, water spray, foam, impacts, and magical particles. They all use the same runtime rather
than bespoke effect classes.

## Simulation and budgets

`CpuParticleSystem` owns a pre-reserved dense pool, a bounded event queue, and generation-safe
retained emitters. Burst events and lifetime/rate emitters share the same spawn path. Each request
has an explicit seed, and the system processes events, emitters, particles, and removals in stable
order. The overflow policy is drop-new: the pool never reallocates past its configured maximum.
Global and per-prototype live/spawn budgets, LOD rejections, collision counts, and drops remain
visible in telemetry.

Simulation supports world or emitter-local space, profile wind response, gravity, drag, inherited
velocity, and optional depth/voxel collision callbacks. Callers own collision queries because the
particle system is renderer infrastructure and does not reach into gameplay world state. A
collision response uses the authored radius and restitution. Effects without a supplied query
remain non-colliding rather than inventing geometry.

Exact `WorldPosition` anchors and camera-relative presentation keep effects stable across
floating-origin shifts. Dense in-place compaction avoids per-frame allocation. Previous/current
positions support interpolation; size, color, roll, and atlas frame derive from normalized age.
Particle serials are presentation identities only and do not enter saves or authoritative
replication.

## Presentation

`ParticlePresentation` uses shared geometry and the retained scene's material/layer instance
batching. Billboard particles can face the camera or align to velocity; mesh particles select an
authored mesh group. Prototype metadata selects lit, unlit, or emissive response and alpha,
additive, or premultiplied-alpha blending. Flipbook columns, rows, frame count, and rate are carried
per instance.

Soft particles declare a fade distance and sample the graph-owned `scene_depth_copy`. The SSAO
pass writes this copy as a second color target, so transparent particles can fade at opaque
intersections without sampling an attachment that is concurrently bound for depth. The native
backend allocates graph descriptor sets per frame and per pass; later resource bindings therefore
cannot invalidate a set already referenced by earlier commands in the same command buffer.

Presentation capacity is independently bounded and reports drops. Render-object child counts make
unparented particle removal constant-time even when thousands expire together.

## Related transient systems

`TrailRenderer` turns generation-safe streams of exact world points into bounded, camera-facing
ribbon segments. Width, material group, blend layer, color, emissive strength, minimum point
distance, segment lifetime, and per-trail segment count are explicit.

`SurfaceMarkRenderer` is the surface-mark/decal-equivalent path. Validated prototypes control
material grouping, blending, size range, atlas variants, lifetime/fade, surface offset, distance,
and lighting. Spawn requests supply a surface position, normal, color, rotation, scale, and
deterministic seed. Marks use shared quad geometry and instanced draws.

See [environment effect authoring](../authoring/environment_effects.md) for formats and integration.

## Measurement

`RendererStats` and benchmark JSON expose CPU update time, presentation synchronization time,
active particles/emitters, material groups, and drops. The `particles` scene remains the isolated
50,000-particle stress workload. The `starting-biome` scene is the integrated weather, smoke,
embers, vegetation, water, atmosphere, and lighting workload.

```sh
build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene particles --radius 0 --warmup 60 --frames 300 \
  --output build/default-release/particle-benchmark.json
```

Effects are visual responses. Anything that affects gameplay originates from an authoritative
gameplay event; its particle, trail, or mark remains presentation-only.
