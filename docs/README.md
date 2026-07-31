# Heartstead documentation

This directory contains maintained documentation for the current engine, its long-term contracts,
and contributor workflows. The root [README](../README.md) is the shortest entry point; this page
is the map for everything else.

## Start here

- [Project status](project_status.md) — audited implementation snapshot and known limits.
- [Build instructions](dev/build_instructions.md) — configure and compile the repository.
- [Running Heartstead](dev/running.md) — applications, controls, saves, and multiplayer.
- [Testing](dev/testing.md) — test presets, sanitizers, smoke tests, and verification expectations.
- [Asset pipeline](asset_pipeline.md) — source assets, cooking, runtime formats, and contributor
  guidance.
- [Asset Lab](asset_lab.md) — production-cooked asset and presentation inspection.
- [Architecture overview](architecture/overview.md) — ownership boundaries and data flow.
- [Engine specification](architecture/engine_spec.md) — normative long-term engine contract.

## Document types

### Normative contracts

The [engine specification](architecture/engine_spec.md) defines non-negotiable architecture
constraints. Architecture decision records explain why foundational choices were made:

- [ADR 0001: language and build system](adr/0001-language-and-build-system.md)
- [ADR 0002: world model](adr/0002-world-model.md)
- [ADR 0003: modding lifecycle](adr/0003-modding-lifecycle.md)
- [ADR 0004: workpiece microvoxel model](adr/0004-workpiece-microvoxel-model.md)

ADRs are historical records. Do not rewrite an accepted ADR to make it look current; supersede it
with a new ADR when the decision changes.

### Maintained architecture

These pages describe current subsystem boundaries. They should explain ownership, invariants,
data flow, failure behavior, and extension points rather than repeat every class name.

**Runtime and infrastructure**

- [Architecture overview](architecture/overview.md)
- [Runtime composition](architecture/runtime_composition.md)
- [Platform](architecture/platform.md)
- [Jobs](architecture/jobs.md)
- [Math](architecture/math.md)
- [Debug inspection](architecture/debug_inspection.md)
- [Replay](architecture/replay.md)

**World and simulation**

- [World model](architecture/world_model.md)
- [World state](architecture/world_state.md)
- [World snapshots](architecture/world_snapshot.md)
- [Chunks](architecture/chunks.md)
- [Dirty regions](architecture/dirty_regions.md)
- [Voxel palette](architecture/voxel_palette.md)
- [Voxel lighting](architecture/voxel_lighting.md)
- [Voxel fluids](architecture/voxel_fluids.md)
- [World generation](architecture/worldgen.md)
- [Region graph](architecture/region_graph.md)
- [Simulation LOD](architecture/simulation_lod.md)
- [Processes](architecture/processes.md)
- [Rooms](architecture/rooms.md)
- [Spatial networks](architecture/spatial_networks.md)

**Content and gameplay-facing representations**

- [Prototype registry](architecture/prototype_registry.md)
- [Prototype semantic validation](architecture/prototype_semantic_validation.md)
- [Modding](architecture/modding.md)
- [Scripting](architecture/scripting.md)
- [Resource packs](architecture/resource_packs.md)
- [Entities](architecture/entities.md)
- [Items and cargo](architecture/items_cargo.md)
- [Workpieces](architecture/workpieces.md)
- [Build pieces and assemblies](architecture/build_pieces_and_assemblies.md)
- [Commands](architecture/commands.md)
- [Transactions](architecture/transactions.md)

**Presentation and assets**

- [Assets](architecture/assets.md)
- [Rendering](architecture/rendering.md)
- [Environment rendering](architecture/environment_rendering.md)
- [Chunk meshing](architecture/chunk_meshing.md)
- [Animation](architecture/animation.md)
- [Particles](architecture/particles.md)
- [Audio](architecture/audio.md)
- [Game UI](architecture/game_ui.md)

**Multiplayer and persistence**

- [Networking](architecture/networking.md)
- [Replication](architecture/replication.md)
- [Save format](architecture/save_format.md)
- [Save database](architecture/save_database.md)
- [Save snapshots](architecture/save_snapshot.md)
- [Save binary codec](architecture/save_binary_codec.md)
- [Save text codec](architecture/save_text_codec.md)
- [Save migrations](architecture/save_migrations.md)

### Authoring and inspection

- [Asset pipeline](asset_pipeline.md)
- [Asset conventions](asset_conventions.md)
- [Asset Lab](asset_lab.md)
- [Terrain material authoring](terrain_material_authoring.md)
- [Vegetation authoring](authoring/vegetation.md)
- [Environment effect authoring](authoring/environment_effects.md)

### Operations and measurements

- [Build instructions](dev/build_instructions.md)
- [Running Heartstead](dev/running.md)
- [Testing](dev/testing.md)
- [Renderer benchmarks](performance/renderer_benchmarks.md)

## Maintenance rules

1. Update documentation in the same change as the behavior or contract it describes.
2. Use present tense only for behavior verified in the current source or tests.
3. Keep status in [project status](project_status.md), not in architecture documents.
4. Do not hard-code a total test count. The suite changes often; show commands that enumerate it.
5. Keep command examples copyable and verify executable names and option spelling against source.
6. Mark benchmark data with date, commit, build type, machine, backend, and run configuration.
7. Keep one maintained page per concern. Delete superseded milestone and acceptance snapshots after
   their durable conclusions have moved into the appropriate guide; Git history preserves them.
8. Link to a more detailed subsystem page instead of duplicating long implementation inventories.
9. Treat `--help`, CMake presets, manifests, schemas, and tests as executable sources of truth.
10. Call out security, persistence, platform, and compatibility limits explicitly.
