# Developer worlds and scenario authoring

Developer worlds are ordinary `kind = "scenario"` prototypes discovered through mod content
validation. `DeveloperWorldRegistry` selects definitions with `developer_world = "true"`, retains
their stable namespaced prototype IDs, and provides category/search browser data plus a normal
`SessionLaunchRequest`. Adding a data-only developer world does not require editing the menu or a
central scenario list.

Unit tests, headless integration tests, benchmarks, Asset Lab, the cooker, and diagnostic programs
remain separate targets. Only worlds intended for interactive exploration belong in this registry.

## Definition fields

The scenario prototype supports:

- `id`, `display_name`, and `description` for stable identity and browser text;
- `developer_world`, `category`, `tags`, and optional `thumbnail` for discovery;
- `world_source`: `generated`, `packaged_fixture`, `existing_save`, or
  `deterministic_setup`;
- `persistence_policy`: `ephemeral`, `temporary_copy`, or `persistent`;
- `world_seed`, `generator_preset`, `start_region`, and `spawn_mode`;
- optional exact `spawn_position = "x:y:z"`, `spawn_yaw`, and `spawn_pitch`;
- `starting_items`, `starting_cargo`, and data-authored `scene_entities`;
- `initial_world_time` and `initial_weather`;
- `required_mods`, `required_resource_packs`, and `enabled_gameplay_modules`;
- `debug_defaults`, optional `setup_hook`, and optional `benchmark_profile`.

Comma-separated IDs use stable local or namespaced identifiers. Scene entities use
`prototype@x:y:z:yaw:uniform_scale`. Large-coordinate spawn values materialize as normalized
`WorldPosition` values; authoritative positions are not collapsed into floats.

## Persistence policies

- **Ephemeral** worlds never receive a save path and are discarded at teardown.
- **Temporary copy** worlds materialize their packaged fixture into session-owned state and are
  discarded at teardown unless a later explicit save-as flow promotes them.
- **Persistent** worlds are intended for a save location supplied by the world-management flow.

Developer scenarios should normally be ephemeral or temporary-copy. The browser exposes the
policy so a developer never mistakes a disposable fixture for a normal save.

## Setup hooks

Prefer `starting_items`, `starting_cargo`, `scene_entities`, seed/generator fields, and ordinary
content data. Use a setup hook only for deterministic fixtures that would be unreasonable to spell
out voxel-by-voxel or require a specialized invariant.

Hooks live in `game/scenarios/scenario_setup.*`, are registered by stable local ID, receive only the
authoritative `WorldState` and immutable `VoxelPalette`, and must be deterministic for the same
input metadata. They run during server world construction before physics/collision systems are
created and before ticking starts. Missing hooks fail the launch; they never silently fall back to
another world.

The `renderer_proof` hook is shared by the menu world and automated developer-world test. It
materializes the former retained-renderer proof terrain around chunk coordinates
`(1,000,000,000, 0, -1,000,000,000)`, preserving its floating-origin coverage without making the
standalone render smoke executable the normal gameplay path.

## Current entries

- `base:scenarios/foundation_slice` — generated, Gameplay, ephemeral; the current interactive
  gameplay-development slice.
- `base:scenarios/renderer_proof` — packaged deterministic fixture, Rendering, temporary-copy;
  large-coordinate renderer/streaming proof world.

`base:scenarios/homestead` remains the normal generated world definition and is not listed as a
developer world.

## Authoring checklist

1. Add a scenario prototype below a mod's `data/scenarios` directory.
2. Give it a stable namespaced ID, browser text, category, world source, and persistence policy.
3. Set `developer_world = "true"` only for an interactive developer world.
4. Prefer declarative entities/content; register a deterministic hook only when necessary.
5. Run `heartstead_developer_world_tests` and the content validation tests.
6. Keep automated performance assertions in benchmark/test targets even when they reuse the same
   fixture.
