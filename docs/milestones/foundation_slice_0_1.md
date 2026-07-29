# Foundation Slice 0.1

Status: in progress

The Foundation Slice is the first small but real Heartstead game build. It proves that the
authoritative runtime, voxel world, controller, rendering, asset, presentation, audio, particle,
and persistence paths form one dependable platform.

It is not an engine-completeness milestone. Farming, crafting, enemies, buildings, inventory
drops, progression, a universal editor, full PBR, material graphs, and comprehensive hot reload
are outside this slice.

## Acceptance rules

- Every checkbox describes an observable result, not an implementation task.
- `[Automated]` items must have a named test or smoke command.
- `[Native]` items require the real windowed renderer, input, audio, or a visual judgment.
- `[Both]` items need an automated invariant plus a native verification.
- A feature is not accepted solely because an isolated subsystem test passes. Its path through
  `dev_game` must also work when the item is marked `[Both]`.
- A rejected authoritative command must not produce success feedback.
- The slice remains runnable at every milestone boundary.

## Baseline

Recorded 2026-07-29 before Foundation Slice implementation:

```text
cmake --build --preset default-debug -j2
ctest --test-dir build/default-debug --output-on-failure -j2
./build/default-debug/apps/dev_game/heartstead_dev_game --frames 3
```

Result:

```text
Build: passed
Tests: 95/95 passed
dev_game headless smoke: passed
```

The first partial-target test attempt was intentionally not treated as the baseline: three test
executables had not been built and the dedicated-server smoke binary was stale. A complete preset
build followed by the commands above produced the authoritative result.

## Application and session

- [x] `[Automated]` `dev_game --frames 3` always creates a valid local authoritative session.
- [x] `[Automated]` a bounded remote-client smoke can connect to the dedicated server.
- [x] `[Automated]` headless `--frames` runs do not require a display or audio output device.
- [ ] `[Native]` the native executable creates the window, renderer, audio system, input system,
      and development mode without startup warnings.
- [x] `[Automated]` bounded shutdown releases the gameplay mode before application-owned renderer,
      audio, window, and platform resources.
- [x] `[Automated]` `main.cpp` contains launch configuration and composition only; gameplay
      features are not manually integrated there.
- [ ] `[Native]` the diagnostic overlay shows session mode, connection state, authoritative tick,
      presentation tick, and last runtime error.

## Deterministic Foundation environment

- [x] `[Automated]` a new Foundation world records a stable scenario ID, layout version, and seed.
- [x] `[Automated]` repeated new-world construction produces byte-equivalent baseline chunks.
- [ ] `[Both]` the player appears at the documented safe spawn.
- [x] `[Automated]` the full player capsule and camera pivot volume at spawn contain no solid voxel.
- [ ] `[Both]` dirt, stone, grass, water, and visually distinct diagnostic blocks are present.
- [ ] `[Both]` the compact controller course contains full-block steps, partial-height steps,
      voxel-terrain inclines, ledges, a low ceiling, a small hole, a landing area, and water.
- [ ] `[Both]` the course crosses at least one chunk boundary at a marked edit station.
- [ ] `[Native]` daylight, shadows, fog, and the basic sky render in the Foundation world.
- [x] `[Automated]` saved edit deltas are applied after deterministic baseline construction.

For this slice, a slope is voxel terrain: a terraced incline composed from full and
partial-height voxel shapes. It is not an arbitrary rotated plane or hidden physics fixture.

## Player controller and camera

- [ ] `[Both]` the player walks with stable acceleration and stopping.
- [ ] `[Both]` sprint changes locomotion speed when requested.
- [ ] `[Both]` jump starts only from a valid supported state.
- [ ] `[Both]` the player falls and lands without penetration or persistent hovering.
- [ ] `[Both]` the player steps onto the supported normal voxel ledge height.
- [ ] `[Both]` the player traverses the ascending and descending voxel-terrain incline.
- [x] `[Automated]` an unsupported ledge or ceiling prevents an invalid step-up.
- [x] `[Automated]` traversing internal block edges does not create unrequested upward velocity.
- [ ] `[Both]` removing the supporting block makes the player fall after collision refresh.
- [ ] `[Both]` water enters and exits the existing swimming/controller mode correctly.
- [ ] `[Native]` first- and third-person camera modes can be selected explicitly.
- [ ] `[Both]` the third-person camera shortens its boom before entering solid terrain.
- [ ] `[Native]` the camera restores its desired distance smoothly after an obstruction clears.
- [ ] `[Native]` the controller overlay displays position, velocity, requested movement, actual
      displacement, grounded state, ground normal/state, controller mode, step result, supporting
      body/block, and pending terrain-collision revision.
- [ ] `[Native]` controller geometry and contact debug drawing can be toggled independently of the
      text overlay.

## Voxel selection and authoritative editing

- [ ] `[Both]` aiming at a selectable voxel produces a hit position and face.
- [ ] `[Native]` the selected voxel's declared selection bounds receive a clear outline.
- [ ] `[Native]` the outline disappears when no voxel is in interaction range.
- [ ] `[Both]` primary action submits a remove command for the selected voxel.
- [ ] `[Both]` secondary action submits a place command against the selected hit face.
- [x] `[Automated]` the server rejects an edit outside interaction range.
- [x] `[Automated]` the server rejects removal of an empty voxel.
- [x] `[Automated]` the server rejects placement into an occupied voxel.
- [x] `[Automated]` the server rejects placement intersecting an authoritative player capsule.
- [x] `[Automated]` the server rejects an edit targeting an unloaded chunk.
- [x] `[Automated]` the server accepts a valid edit and records exactly one semantic accepted event.
- [x] `[Automated]` a rejected edit records no accepted event.
- [x] `[Automated]` rapid duplicate removal accepts once and rejects subsequent empty-target edits.

## Replication and derived terrain updates

- [x] `[Automated]` an accepted server edit changes the authoritative chunk content revision.
- [ ] `[Both]` the accepted edit appears in the local client voxel world.
- [x] `[Automated]` the accepted edit appears on a second connected client.
- [x] `[Automated]` the client applies the voxel mutation before dispatching its accepted-edit
      presentation event.
- [ ] `[Both]` an interior edit rebuilds the changed chunk mesh.
- [ ] `[Both]` a face-boundary edit rebuilds both loaded neighboring chunk meshes.
- [x] `[Automated]` collision cooking reaches the authoritative edited chunk revision.
- [ ] `[Both]` character collision reflects removal and placement after the collision update.
- [x] `[Automated]` an edit dirties voxel lighting when the previous or current block affects light.
- [x] `[Automated]` an edit activates fluid work for the edited block and its relevant neighbors.
- [ ] `[Native]` diagnostics show command rejection, failed replication apply, failed remesh,
      failed collision cooking, and unresolved lighting/fluid work.
- [x] `[Automated]` public renderer diagnostics expose pending, failed, and stale terrain work plus
      failed lighting jobs without requiring access to renderer internals.

## Accepted edit feedback

- [ ] `[Both]` a successful removal emits the previous voxel's break particle.
- [ ] `[Both]` a successful removal plays the previous voxel's positional break sound.
- [ ] `[Both]` a successful placement plays the placed voxel's positional placement sound.
- [x] `[Automated]` feedback is queued only after the accepted event reaches presentation.
- [ ] `[Both]` rejected removal and placement produce no success particle or sound.
- [x] `[Automated]` a missing particle or sound reference resolves to its named fallback and emits
      one clear diagnostic rather than failing the session.

## Asset Pipeline V1

- [x] `[Automated]` the Foundation manifest closes over every transitive asset dependency.
- [x] `[Automated]` filtered model cooking also cooks required external or generated image assets.
- [x] `[Automated]` changing a dependency changes the dependent cooked record/hash.
- [x] `[Automated]` PNG and JPEG sources cook to versioned RGBA8 texture assets.
- [x] `[Automated]` base-color texture RGB is uploaded as sRGB and material factors remain linear.
- [ ] `[Both]` one textured static glTF/GLB prop renders through the catalog and cooked store.
- [ ] `[Both]` one textured skinned player glTF/GLB renders through the same path.
- [ ] `[Both]` per-primitive base-color material assignment is preserved and visible.
- [ ] `[Both]` opaque and alpha-mask material modes behave correctly.
- [ ] `[Both]` double-sided material state behaves correctly.
- [x] `[Automated]` unsupported blend materials fail cooking with the logical asset ID and reason.
- [x] `[Automated]` animation mappings resolve unique authored clip names, never numeric indices.
- [x] `[Automated]` missing or duplicate mapped animation names produce a clear validation error.
- [x] `[Automated]` WAV and Ogg Vorbis sources cook to the versioned runtime audio representation.
- [ ] `[Both]` at least one positional emitter plays a cooked sound asset.
- [x] `[Automated]` an unregistered runtime sound event resolves to the named fallback and the real
      audio backend plays its production-cooked payload.
- [x] `[Automated]` runtime model, texture, material, animation, and audio resources are cached by
      stable logical asset ID.
- [ ] `[Both]` missing texture, material, model, animation, and sound assets each use their named
      fallback behavior.
- [ ] `[Native]` load diagnostics identify logical ID, source/cooked path, failing dependency, and
      fallback used.

## Data-driven presentation

- [x] `[Automated]` every entity present in the Foundation scene resolves an `entity_visual`
      definition.
- [x] `[Automated]` declaring a static entity visual does not require a skin or animation mapping.
- [x] `[Automated]` declaring a skinned entity visual validates all named animation mappings.
- [ ] `[Both]` the player selects idle, walk, run, jump, fall, and swim presentation states from
      semantic locomotion state.
- [x] `[Automated]` newly replicated entities with valid visual definitions appear without
      application-entry-point changes.
- [x] `[Automated]` removed entities release their presentation instances while cached assets remain
      valid for other users.
- [ ] `[Both]` footstep sounds follow locomotion timing and resolve from the supporting voxel.
- [x] `[Automated]` an unknown visual definition presents the fallback model/material and reports
      the unresolved visual ID once.

## Persistence

- [x] `[Automated]` a missing Foundation save slot creates a new deterministic world.
- [x] `[Automated]` an existing Foundation save slot reopens instead of creating another baseline.
- [x] `[Automated]` player position survives clean save and restart.
- [ ] `[Both]` removed and placed voxels survive clean save and restart.
- [x] `[Automated]` edits on both sides of a chunk boundary survive restart.
- [x] `[Automated]` the active voxel palette manifest is preserved and validated on load.
- [x] `[Automated]` a missing voxel prototype follows the existing missing-prototype recovery path.
- [x] `[Automated]` an incompatible Foundation layout version fails clearly rather than applying
      edit deltas to an unrelated baseline.
- [x] `[Automated]` periodic dirty save and clean-shutdown save use the transactional generation
      commit path.

## Regression gates

- [x] `[Automated]` the complete default-debug build passes.
- [x] `[Automated]` the complete default-debug CTest suite passes.
- [x] `[Automated]` the Foundation headless integration test starts a session, spawns a player,
      submits an edit, advances ticks, verifies server and client state, saves, reloads, and
      verifies the edit.
- [x] `[Automated]` the Foundation asset validation test cooks and loads all required assets and
      resolves every presentation reference.
- [x] `[Automated]` the bounded `dev_game` headless smoke passes.
- [x] `[Native]` the bounded real-window smoke creates renderer/audio resources, renders the
      Foundation scene, and shuts down cleanly.
- [ ] `[Native]` all visual and audible checklist items receive a final in-game pass before the
      milestone is marked complete.

## Automated evidence

Recorded 2026-07-29:

- `cmake --build --preset default-debug -j2 --target heartstead_dev_game`: passed.
- `ctest --test-dir build/default-debug --output-on-failure -j2`: 100/100 passed.
- `heartstead_dev_game --native-frames 120 --no-save`: passed after waiting for the final GPU
  submission before mode-owned render resources are released. The host does not provide
  `VK_LAYER_KHRONOS_validation`, so the separate warning-free startup item remains open.
- `heartstead_dev_game_save_smoke` creates a missing slot, verifies a periodic generation commit,
  clean-shutdown commit, and staged-generation cleanup, then reopens and recommits the same slot.
- `heartstead_runtime_spine_tests`
  - `test_external_listen_runtime_uses_true_remote_endpoint`
  - `test_two_remote_clients_predict_and_interpolate`
  - `test_typed_voxel_commands_validate_and_replicate`
  - `test_boundary_voxel_edit_rebuilds_collision_and_removes_support`
  - `test_session_save_and_reload_restores_authoritative_state`
  - `test_boundary_voxel_edits_survive_restart`
  - `test_foundation_save_rejects_incompatible_layout`
  - `test_session_file_load_preserves_missing_prototypes`
  - `test_session_load_restores_persisted_missing_voxel_palette`
  - `test_authoritative_player_input_moves_and_replicates` verifies the local prediction path
    retains the latest controller diagnostics exposed to the Foundation overlay
  - `test_client_command_result_history_is_bounded` verifies long edit sessions retain only the
    newest 256 accept/reject diagnostics and report every discarded oldest result
- `heartstead_movement_controller_tests`
  - `test_snapshot_prediction_camera_and_load` verifies first-/third-person body visibility,
    immediate collision-safe boom retraction, and gradual time-based restoration to the desired
    distance after the voxel obstruction clears
  - `test_walk_jump_dash_and_step` verifies controller tick diagnostics preserve the collision
    solver's requested/applied displacement and report a voxel-native partial-block step
- `heartstead_engine_tests`
  - `test_filtered_model_dependency_cooking`
    - selects the model's external buffer and image dependency closure
    - proves an external image change alters the dependent model's cooked hash
    - verifies alpha-blend rejection includes the logical model ID and importer reason
  - `test_resource_pack_discovery_and_asset_catalog` validates the versioned WAV and Ogg Vorbis
    production payloads and rejects malformed or non-Vorbis Ogg input
- `heartstead_audio_system_tests`
  - production-cooks WAV and procedural-tone fixtures, deletes both sources, plays the cooked
    payloads (including a positional tone), and verifies stable logical-ID cache reuse
  - verifies a missing event resolves to the configured named fallback, warns once per missing ID,
    exposes fallback counters, and plays the fallback's cooked payload through miniaudio
- `heartstead_renderer_frontend_tests`
  - `test_renderer_frontend_submits_headless_frames` verifies model base-color textures use an
    sRGB image view while base-color factors reach the GPU surface-material record unchanged, and
    repeated logical model ids reuse material handles and texture-array layers
  - verifies public renderer stats preserve terrain queue/failure/stale counters and failed
    lighting-job counts used by the Foundation diagnostic overlay
- `heartstead_mesh_manager_tests` verifies repeated stable ids share one mesh allocation, retain
  explicit ownership, and retire only after the final release
- `heartstead_animated_model_presentation_tests` verifies two presenters can share one model mesh
  and that shutting either presenter down leaves the other one's mesh valid
- `heartstead_voxel_interaction_presentation_tests`
  - `test_only_accepted_edits_emit_data_driven_feedback`
  - `test_foundation_voxels_resolve_feedback_resources`
- `heartstead_model_asset_tests`
  - `test_typed_gltf_import_and_codec`
  - `test_base_storybook_player_asset`
- `heartstead_model_presentation_system_tests` validates cooked manifest closure, every declared
  visual model, every named clip and sound reference, one decoded model and one renderer mesh set
  for two definitions sharing a logical model id, static/skinned insertion, removal, and
  one-diagnostic-per-ID fallback presentation for an unknown visual.
- `heartstead_headless_session_tests`
  - `test_foundation_scene_objects_resolve_visual_definitions`

Unchecked `[Both]` and `[Native]` items intentionally remain open until their in-game portion has
been observed.

## Gameplay-additive exit condition

The slice is complete only when:

- adding a terrain block requires its voxel prototype, material/texture references, collision and
  rendering properties, sound references, and optional particle reference, but no renderer code;
- adding an entity requires its gameplay prototype, visual definition, model/materials, animation
  mapping, and sound set, but no manual presenter in `dev_game`;
- adding an interaction requires an input/request, authoritative command and validation, state
  change, and presentation event, but no effect trigger in `main.cpp`.
