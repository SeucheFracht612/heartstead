# Modding Architecture

Mods define game meaning. The base game uses the same pipeline as community mods.

Current implemented foundation:

- one-directory-per-mod discovery under `mods/`
- required `mod.toml`
- namespace-style mod ids
- bounded, deterministic directory traversal that rejects symlinked mod/manifest/content entries,
  excessive depth, excessive entry counts, and filesystem errors
- one bounded flat-manifest parser shared by mod manifests, generic prototypes, prototype patches,
  and resource-pack manifests; it handles comments only outside quoted strings, decodes the
  supported quoted escapes, preserves the first value on duplicate keys, and reports malformed,
  duplicate, oversized, and unknown manifest fields instead of silently accepting them
- optional `dependencies = "other_mod,base"` manifest field with deterministic dependency
  ordering and cycle/missing-dependency diagnostics
- engine-owned mod lifecycle plan with deterministic stage order for settings, prototypes,
  data updates, final fixes, assets/resources, migration scripts, runtime server scripts, and
  runtime client scripts
- generic `*.prototype.toml` loading under `data/`
- generic `*.prototype_patch.toml` data-update patches with `target` and `set.<field>` keys
- generic `*.final_patch.toml` final-fix patches applied after data updates
- prototype ids in `namespace:local_id` form
- deterministic per-mod prototype fingerprints for save compatibility metadata, including patch
  declarations
- prototype registry lookup and expected-kind validation
- first-class scenario prototypes for game/session startup data
- voxel palette construction from `voxel` prototypes for terrain cell ids
- first-class material prototypes for renderer-facing shader templates and texture parameters
- shared `ModValidation` report for discovery, lifecycle planning, prototype loading, prototype
  patching, and registry checks
- aggregate content validation that combines mods, resource packs, asset indexing, material
  definitions, lifecycle planning, and material asset-reference checks
- semantic validation for engine-owned representation fields
- script runtime boundary with a build-disabled backend and the pinned production Luau
  compiler/VM, isolated per-mod/stage VMs, hard resource limits, registered host API/event
  validation, and ordered host event intake records for emitted events
- lifecycle-classified script files are materialized into validated script module descriptors
  with stable module ids, stages, declared permissions, and API versions
- game runtime startup validation consumes the same report and requires `mods/base`
- `heartstead_mod_validator` and `heartstead_prototype_inspector` tools

Prototype loading is deliberately generic at the engine layer. Game/runtime systems own
the semantics of item, voxel, build piece, workpiece, crop, animal, machine, room, ward,
and process prototypes.

Mods are returned in dependency order before prototype loading and staged prototype
patches run. This gives later mods a stable way to patch prototypes owned by prerequisite
mods without relying on directory traversal order.

`ModLifecyclePlanner` derives an inspectable stage schedule from the discovered mod list.
The schedule is carried through `ModValidationReport` and aggregate content validation,
is renderable through debug inspection, and is printed by `heartstead_mod_validator`.
The schedule is grouped by engine stage while preserving dependency order within each
stage. Current task kinds include:

```text
settings           mod.toml
prototypes         data/**/*.prototype.toml
data_updates       data/**/*.prototype_patch.toml
final_fixes        data/**/*.final_patch.toml
assets             assets/** and locale/**
migration          migrations/** and scripts/migration[s]/**/*.lua[u]
runtime_server     scripts/runtime_server/**/*.lua[u]
runtime_client     scripts/runtime_client/**/*.lua[u]
```

Script files outside a known script stage produce validation diagnostics. This prevents
runtime or migration scripts from silently landing in the wrong sandbox stage later.
Stage recognition is anchored to the top-level mod layout, so a nested directory merely named
`scripts`, `data`, or `assets` cannot impersonate an engine lifecycle stage.

Prototype patch loading has two deterministic passes after all new prototype definitions
are loaded and before registry semantic validation:

```text
data_updates       *.prototype_patch.toml
final_fixes        *.final_patch.toml
```

Both patch stages use the same small contract. Patches can set representation fields, but
they cannot change immutable prototype identity fields such as `id` or `kind`. A mod that
patches another mod's prototype must declare that target mod in `dependencies`.
Malformed prototype and patch files are not materialized, and each patch is applied atomically so
an invalid immutable-field edit cannot partially change other fields or increment the applied
patch count.

The engine validates representation-level fields that affect core boundaries, such as
item stack limits, cargo mass and transport modes, entity kind, build-piece tags,
assembly part references, workpiece grid shape/material references, process duration,
room descriptor codes/severity, material shader/template fields, scenario startup references,
and voxel palette fields. Gameplay-specific balance and behavior still belong in game/runtime
systems and mods.

Resource packs are discovered separately from gameplay mods. They can override
presentation assets through the asset catalog without changing prototype meaning.
The aggregate content validator uses that active asset catalog to catch material
shader/template references that cannot be satisfied by the enabled content set.
`heartstead_mod_validator --inspect` prints the same aggregate content validation report through
debug inspection, including mod/resource-pack/prototype/script/asset counts, resource-pack load
priorities, material asset overrides, and structured diagnostics.

Implemented lifecycle stages:

```text
settings stage
prototype/data stage
data updates stage
final fixes stage
asset/resource stage
migration stage
runtime server stage
runtime client stage
```

Runtime server/client/migration script stages run through the production Luau sandbox with
data-only event emission. Lifecycle planning classifies those script files, mod validation
materializes them into `ScriptModuleDesc` records, and Luau-enabled validation compiles and
bounded-initializes them before content is accepted. Each mod/stage pair owns an isolated VM and
each module receives its own sandbox environment. No gameplay mechanic is supplied by this
engine-level boundary.
