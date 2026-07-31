# Environment effect authoring

Heartstead's environment effects are content records backed by bounded production runtimes. Prefer
adding or tuning prototypes over adding a dedicated renderer path for an individual visual.

## Particle prototypes

Particle files use `kind = "particle"` and a stable namespaced id:

```toml
kind = "particle"
id = "base:particles/rain_drop"
display_name = "Rain Drop"
material_group = "2"
lifetime_min_seconds = "0.45"
lifetime_max_seconds = "0.9"
speed_min = "13.0"
speed_max = "21.0"
direction_spread = "0.04"
gravity = "-6.0"
drag = "0.04"
size_min = "0.018"
size_max = "0.032"
end_size_multiplier = "1.0"
start_color = "0.54,0.72,0.90,0.62"
end_color = "0.38,0.58,0.78,0.22"
blend_mode = "premultiplied_alpha"
shading = "unlit"
alignment = "velocity"
velocity_stretch = "1.8"
wind_response = "0.35"
collision = "depth"
collision_radius = "0.01"
lod_start_distance = "22"
lod_end_distance = "54"
maximum_live_particles = "18000"
spawn_budget_per_update = "2400"
priority = "2"
```

Supported presentation values are:

- `blend_mode`: `alpha`, `additive`, or `premultiplied_alpha`;
- `shading`: `lit`, `unlit`, or `emissive`;
- `geometry`: `billboard` or `mesh`, with `mesh_group` for mesh particles;
- `alignment`: `camera` or `velocity`;
- `simulation_space`: `world` or `local`;
- `collision`: `none`, `depth`, or `voxel`.

Use `atlas_columns`, `atlas_rows`, `atlas_frame_count`, and `atlas_frames_per_second` for a
flipbook. `emissive_intensity`, `soft_fade_distance`, `velocity_stretch`, `wind_response`,
`collision_radius`, and `collision_restitution` are optional effect controls. LOD start/end,
per-prototype maximum live count, spawn budget, and priority must describe the intended production
cost. System-wide budgets still take precedence.

Every burst carries an explicit deterministic seed. Persistent sources should use retained
emitters; weather uses `WeatherEffects`, which maps evaluated rain, snow, ash, and spores onto base
prototypes inside a camera-following volume. Supply depth/voxel collision callbacks from the
runtime that owns the relevant geometry.

## Surface-mark prototypes

Surface marks use `kind = "decal"`:

```toml
kind = "decal"
id = "base:decals/soot"
display_name = "Soot"
material_group = "1"
blend_mode = "premultiplied_alpha"
size_min = "0.25"
size_max = "0.8"
lifetime_seconds = "0"
fade_seconds = "0"
surface_offset = "0.004"
maximum_distance = "96"
atlas_columns = "2"
atlas_rows = "2"
atlas_frame_count = "4"
receives_lighting = "true"
```

Gameplay supplies a world position and normalized surface direction when it spawns a mark. Keep
`surface_offset` just large enough to avoid z-fighting. A zero lifetime means retained until
explicit clearing; otherwise `fade_seconds` applies at the end of life. Atlas selection uses the
spawn seed, so a mark remains stable across frame order and floating-origin changes.

The base catalog includes soot, mud, wetness, moss, cracks, mining marks, footprints, wheel tracks,
combat marks, and magical residue. Reuse those ids when only spawn color, rotation, or scale needs
to vary.

## Trails and ribbons

Trails are runtime presentation objects because their points come from moving gameplay state.
Create a `TrailDesc`, append exact `WorldPosition` points, update with the render camera, and destroy
the generation-safe id when its source ends. Descriptor controls include width, color, material
group, blend layer, emissive strength, minimum point spacing, segment lifetime, and maximum segment
count. Both global and per-trail capacity are bounded; rejected segments are reported in stats.

## Authoring rules

- Keep visuals presentation-only; gameplay state and damage never live in an effect.
- Use exact world anchors and deterministic seeds.
- Prefer premultiplied alpha for smoke, soft precipitation, and surface marks; use additive for
  energy or sparks.
- Give every long-lived or high-rate effect an explicit LOD and budget.
- Use emissive output only for visible radiance. Gameplay creates the associated real light.
- Validate the full mod catalog after adding an effect; invalid enums, ranges, atlases, or budgets
  are source-linked content errors.
