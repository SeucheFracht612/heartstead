# Gameplay Foundations Roadmap: M1–M8

Status: active  
Last reviewed: 2026-07-28

This roadmap turns the remaining engine foundations into a playable, moddable co-op game. It is
ordered by dependency, not by how visible a feature is. Every milestone preserves the existing
ownership rules:

- authoritative state is owned by `ServerRuntime` and `WorldState`;
- runtime handles, backend handles, presentation state, and GPU/audio resources are not saved;
- workers consume immutable snapshots and publish revision-tagged results;
- gameplay and mods use engine boundaries rather than Jolt, miniaudio, Vulkan, sockets, or Luau
  handles;
- headless, dedicated-server, and sanitizer builds remain first-class configurations.

## Cross-milestone delivery rules

1. Land narrow, reviewable commits. A normal slice contains its contract, implementation, tests,
   and architecture update. Mechanical dependency/bootstrap changes may be separate.
2. Keep old deterministic/reference paths where they provide an oracle. Remove a reference path
   only after the replacement has deterministic fixtures and failure coverage.
3. New asynchronous work uses immutable inputs, generation/revision checks, bounded queues,
   cancellation, and owner-thread application.
4. Every budget has a visible backlog/counter and a benchmark fixture. A budget overrun defers
   work; it does not silently turn into an unbounded frame or simulation tick.
5. Save and network formats are versioned before their first incompatible change. Decoders remain
   bounded and fail closed.
6. Each milestone closes with debug/warnings-as-errors, ASan+UBSan, TSan where supported, headless
   integration, save/reload, and relevant renderer benchmarks.

## Dependency order

```text
M1 physics ──────┬──> M3 fluids/swimming
                 ├──> M5 animated physical entities
                 └──> M7 movement prediction/reconciliation
M2 lighting ─────┴──> M5 particles and presentation polish
M4 audio ───────────> gameplay events from M5/M6
M5 entities ────────> M7 remote replicated presentation
M6 UI ──────────────> remote connection/inventory UX
M7 multiplayer ─────> network-facing script behavior
M8 Luau consumes stable gameplay, command, event, and UI contracts from the earlier milestones
```

M2 and M4 can begin once M1 has a stable boundary, but the default execution order remains M1
through M8 so each acceptance scene builds on already verified systems.

## M1 — Real physics with Jolt

### Architecture decisions

- `joltphysics` is a pinned vcpkg manifest dependency. CMake links the imported Jolt target only
  into `heartstead_engine`; public headers continue to expose engine types.
- `PhysicsBackend::headless` remains the deterministic reference/oracle. It is not selected for
  the interactive development game after Jolt is available.
- Every `JoltPhysicsWorld` owns its `PhysicsSystem`, allocators, job system, filters, body map, and
  contact queue. Process-global Jolt registration is reference-counted and hidden in
  `engine/physics/jolt`.
- Engine `PhysicsBodyId` values map to Jolt body IDs; Jolt IDs and pointers never become save,
  entity, or network identity.
- Terrain collision is cooked as chunk-local, greedily merged boxes in an engine-neutral
  `ChunkCollisionShape`. The Jolt backend materializes a `StaticCompoundShape`. This avoids one
  body per voxel and avoids triangle-mesh ghost edges for axis-aligned voxel terrain.
- Collision cooking mirrors meshing: immutable chunk input, chunk identity and content revision,
  bounded worker queue, cancellation, stale-result rejection, and owner-thread body replacement.
  Neighbor chunks are dirtied independently at boundary edits, so the chunk-local box cooker does
  not copy a halo it cannot consume.
- The high-level `PlayerController` remains authoritative for Souls-style acceleration, stamina,
  encumbrance, dash/roll, jump buffering, and mode transitions. Its collision adapter becomes an
  interface. The Jolt implementation owns a `CharacterVirtual`, performs slope limiting,
  stair-walk/floor-stick updates, and reports the same engine-neutral move result.
- World positions are converted into a bounded physics island origin. Only local floats/doubles
  reach Jolt; rebasing never changes stable world coordinates.

### Delivery slices

1. **Dependency and rigid-body backend**
   - pin vcpkg baseline and `joltphysics`;
   - add `HEARTSTEAD_ENABLE_JOLT`, package discovery, compile definition, and capability reporting;
   - implement Jolt lifecycle, layer filters, box/sphere/capsule/compound conversion, body CRUD,
     transforms, velocities, impulses, fixed stepping, sleeping, AABB overlap queries, and contact
     collection;
   - preserve all validation and typed errors at the public boundary;
   - make `physics_sandbox` run one scenario through both backends and compare traces.

2. **Collision shape pipeline**
   - add immutable `ChunkCollisionSnapshot`, greedy axis-aligned box cooker, result contract,
     scheduler, stats, and inspection;
   - enqueue `chunk_collision` dirty regions without consuming mesh/lighting work;
   - replace static bodies atomically only for matching chunk generation/revision;
   - remove collision bodies on unload and reject late results from a previous load generation;
   - include prototype collision bounds and non-colliding/fluid/decorative occupancy.

3. **Character integration**
   - introduce an engine-neutral character collision/controller contract;
   - retain the voxel collision implementation as reference tests;
   - implement Jolt `CharacterVirtual` capsule creation, resize checks, slope limit, step-up,
     stick-to-floor, jump detachment, moving-body impulses, and grounded/contact classification;
   - drive it from the existing movement input/modifier state machine;
   - keep ladder and fluid occupancy as world queries; M1 supplies a swim-mode collision stub and
     M3 supplies buoyancy/current behavior;
   - replicate/present the resulting authoritative `WorldPosition`, never the Jolt transform.

4. **Physical resources and dropped bodies**
   - rebuild saved `PhysicalResourceRecord` bodies on load;
   - synchronize dynamic transforms and sleep transitions after the physics phase;
   - make dropped item/resource prototypes create bounded box/convex bodies;
   - preserve the dynamic → sleeping → frozen → cargo conversion lifecycle;
   - batch body creation/destruction at phase boundaries and expose body/sleep/contact counts.

5. **Runtime and acceptance**
   - select Jolt for `dev_game`, headless for deterministic server fixtures unless a test requests
     Jolt, and no physics presentation resources for dedicated server;
   - add generated terrain, placed build-piece, step/slope/jump, dropped-resource, unload/reload,
     far-origin, and teardown fixtures;
   - document tuning values and debug overlays for shapes, contacts, grounded state, and dirty
     collision backlog.

### M1 verification gates

- Repeated runs of each backend produce identical results for that backend.
- The shared sandbox trace has the same body/contact/sleep state transitions; final dynamic body
  position differs by at most `0.02 m` and velocity by at most `0.05 m/s`.
- Character acceptance: `0.6 m` auto-step, configured slope limit, `1.25 m` jump apex within
  `0.03 m`, no tunnelling through a one-voxel wall at supported movement speeds.
- Default collision budget: at most two chunk cooks and `2.0 ms` owner-thread apply per tick;
  backlog and stale/cancelled counts are inspectable.
- `physics_sandbox`, movement/controller tests, `dev_game` acceptance scene, warnings-as-errors,
  ASan+UBSan, and TSan all pass.

## M2 — Voxel lighting and day/night

### Architecture decisions

- Keep `VoxelCell::light` as a scalar 8-bit derived value. Direct sky and block sources are seeded
  separately into scratch buffers, then combined by maximum into the stored byte. Source removal
  deterministically recomputes the invalidated region, so source provenance is not required in
  permanent chunk state.
- Light is derived/cache state, not a player edit. Relighting advances chunk content revision and
  dirties mesh/replication only when visible stored light changes; save deltas do not record
  reproducible baseline sunlight.
- A relight job consumes one immutable snapshot of all resident voxel/palette/source state and
  emits revision-tagged per-chunk light patches. Snapshot copying is incremental; the complete
  connected field is applied atomically after owner-thread validation. Region/halo narrowing is
  deferred until profiling shows the global deterministic solve is insufficient.
- Sunlight uses column seeding from the highest loaded opaque cell and special downward
  transmission; horizontal/upward spread and block light use a deterministic six-neighbor BFS.
- Missing chunks form a known boundary state, not implicit air. Loading a neighbor invalidates
  border lighting on both sides.

### Delivery slices

1. Define light attenuation/source helpers and palette validation; make world commands derive
   emitter light from prototypes rather than accepting client-authored light.
2. Implement deterministic sunlight column seeding and block-emitter BFS across chunk boundaries.
3. Implement removal/replacement recomputation, border invalidation, stale result rejection, and
   save/load reconstruction.
4. Add `ChunkLightScheduler` with cell/time budgets, queue priority, cancellation, inspection, and
   `RendererStats`/debug overlay fields.
5. Feed propagated voxel light into terrain/rich-object shading and fire point-light proxies.
6. Derive normalized day phase from `WorldClock`; calculate sun direction, illuminance,
   ambient term, horizon/zenith colors, and fog tint in a presentation snapshot.
7. Add a fullscreen sky-gradient draw in the sky phase and pass the same sun/ambient values through
   existing frame constants.

### M2 verification gates

- Tunnel, skylight shaft, chunk-border, opaque/partial absorption, emitter place/remove, and
  load-order fixtures have exact golden light fields.
- Removing an emitter leaves no orphan light; loading chunks in a different order converges to the
  same field.
- A 24-hour time lapse is continuous at wraparound and matches golden dawn/noon/dusk/night values.
- Default owner-thread snapshot work is `4096` copied cells per update and complete-field apply has
  a `2.0 ms` budget. The one-chunk `rapid-edits` reference target is solve p95 below `50 ms` on the
  background worker and zero apply-budget overruns in a warnings-as-errors debug build. The
  renderer benchmark reports solve/apply p50/p95, visited cells, backlog, stale results, and
  changed chunks.

## M3 — Fluids

### Architecture decisions

- Fluid identity is a voxel prototype with `BlockLogicalOccupancy::fluid`. Fluid level, falling
  state, and source flag occupy a documented mask in `VoxelCell::state_bits`; unrelated block
  state bits are rejected for fluid cells.
- Simulation is server-authoritative, integer-only, and two-phase: sorted active cells read an
  immutable tick view, emit transfer proposals, and commit a stable sorted result. Iteration order
  cannot change the outcome.
- Water work is activated by voxel edits and settled lazily through `water_network` dirty regions.
  Active-frontier state is reconstructed from saved fluid cells; it is not an unbounded saved job
  queue.
- Below configured sea level, baseline ocean source cells refill deterministically. Player-created
  water is not infinite unless its prototype/state explicitly says source.
- Meshing is a distinct fluid surface extractor. Top vertices use neighboring levels for height,
  sides close exposed columns, and state-derived flow direction is carried to the fluid shader.

### Delivery slices

1. Add fluid prototype/state helpers, water definition, sea-level worldgen sources, codecs, and
   invariant tests.
2. Implement downward-first level spread, lateral decay, source refill, removal settling, active
   frontier, cross-chunk activation, and per-tick budgets.
3. Persist/reload mid-flow through existing chunk cells/edit deltas and prove resumed simulation
   matches uninterrupted simulation.
4. Add fluid mesh sections, surface heights/normals/flow UV data, transparent sorting/material,
   and chunk-border fixtures.
5. Add character submersion queries, buoyancy, drag, swim acceleration/stamina, enter/leave events,
   and dynamic-body buoyancy for opted-in bodies.
6. Add dam-break and ocean benchmark scenes plus simulation/backlog stats.

### M3 verification gates

- Dam-break golden state is identical across repeated runs, chunk load orders, and save/reload at
  every sampled mid-flow tick.
- Oceans below sea level refill but do not create upward/lateral water above the defined rules.
- Player transitions between ground/air/swim without penetration and can surface/jump out.
- Benchmark target: `32,768` active water cells at 20 Hz with p95 simulation below `2.0 ms` on the
  published reference machine; overflow becomes backlog rather than tick expansion.

## M4 — Audio subsystem

### Architecture decisions

- Applications own `IAudioSystem`, matching platform/renderer ownership. Dedicated/headless
  compositions use a deterministic null backend.
- `miniaudio` is a pinned vcpkg dependency hidden in `engine/audio/miniaudio`.
- Mixing policy is backend-neutral: master/music/SFX/ambient buses, gain ramps, voice priority,
  maximum voices, looping, and listener-relative attenuation are tested without an output device.
- Gameplay emits `AudioEvent` using stable prototype/asset IDs. It never names a file path or owns
  a playback voice.
- Positions remain exact `WorldPosition` until converted relative to the listener/camera floating
  origin.

### Delivery slices

1. Add sound-event prototypes, asset resolution, `IAudioSystem`, null backend, voice IDs, buses,
   listener/emitter updates, and typed errors.
2. Implement deterministic mixer math, gain smoothing, attenuation/cone/pan, voice stealing, and
   offline PCM tests.
3. Integrate miniaudio engine/resource manager for WAV/OGG/FLAC, streaming music, decoded SFX,
   groups, and 3D sounds.
4. Queue device notifications and perform output-device reinitialization on the application owner
   thread; preserve logical voices where possible and fall back to silence without crashing.
5. Route movement/fire/environment events to footsteps, impacts, and ambient loops in `dev_game`.
6. Add audio inspection and mixer/voice/device statistics.

### M4 verification gates

- Null/offline tests have exact gain, pan, attenuation, bus, loop, and voice-priority results.
- `dev_game` has positionally updated footsteps and a streaming ambient loop.
- Device removal/re-add never calls game code from the audio callback, leaks a voice, or crashes.
- Mixer target: 128 active mono SFX voices, 48 kHz stereo, 256-frame blocks below `1.0 ms` p95 on
  the published reference machine.

### M4 completion evidence

- `AudioMixer` and both `IAudioSystem` implementations share the same event, voice, bus, listener,
  emitter, gain-ramp, attenuation, and priority boundary. Dedicated/headless compositions open no
  device; tests can exercise either the logical null backend or miniaudio's real callback graph
  through its null output device.
- The miniaudio 0.11.25 backend decodes short assets, streams authored long-form assets, generates
  bounded `.tone` manifests into deterministic PCM, mirrors buses with sound groups, and performs
  device recovery only from the owner thread. Base sound events provide positional earth
  footsteps and a looping homestead ambient bed in `dev_game`.
- Audio inspection exposes resolved event policy and runtime voice/device counters.
  `heartstead_audio_benchmark` renders the production graph offline; the Release reference result
  on an Intel Core Ultra 7 258V is 0.125 ms average / 0.178 ms p95 for 128 looping mono voices,
  48 kHz stereo, 256-frame blocks, and 1,000 measured blocks.

## M5 — Skeletal animation and particles

### Architecture decisions

- Extend the model cooker from container validation to typed glTF mesh/skin/animation extraction.
  Cooked data uses engine-owned formats and limits; runtime never walks arbitrary glTF JSON.
- Default to GPU skinning: four joint indices/weights per vertex, per-instance palette offset, and a
  frame palette buffer. A CPU reference skinner remains for golden tests and benchmark comparison.
- Authoritative replication sends locomotion state, normalized phase/start tick, and transition
  revision—not bone matrices. Clients interpolate state and evaluate/blend clips.
- Particles are presentation-only unless gameplay explicitly creates an authoritative event.
  Emitters consume typed events; CPU simulation produces billboard instances through the retained
  scene/instance path.

### Delivery slices

1. Cook vertex streams, inverse bind matrices, joint hierarchy, clips, channels, interpolation, and
   bounds with strict count/size/index validation.
2. Add skinned mesh resources, vertex layout, palette allocation/upload, shader path, culling
   bounds, and CPU-vs-GPU golden pose tests.
3. Add animation graph foundation: clip player, normalized time, two-clip blend, masks/root policy,
   idle/walk/swim states, and client interpolation.
4. Replicate locomotion animation state and synchronize third-person player/animal presentation.
5. Add deterministic wandering test-animal behavior through a gameplay module.
6. Add particle prototypes, emitter lifetime/rate/burst, seeded CPU update, billboards/atlas frames,
   pooling, overflow policy, fire embers, block puffs, and splashes.
7. Publish skinned-character and particle benchmark scenes.

### M5 verification gates

- Cooked malformed skin/clip inputs fail closed; golden poses match the CPU reference.
- Two clients observing a third derive the same animation state/phase within one rendered
  interpolation interval.
- Idle/walk/swim transitions blend without a bind-pose frame; test animal locomotion follows speed.
- Particle target: 50,000 active particles below `2.0 ms` CPU update and no more than four draw
  calls per material/atlas benchmark grouping.

### M5 progress evidence

- The bounded glTF cooker, `heartstead.model.v1` codec, clip interpolation/blending, CPU skinning
  oracle, unified GPU-skinned vertex path, palette ring, and retained palette ownership are live.
- Controller snapshot v2 replicates idle/walk/swim phase and transition state. Generic animated
  entities use bounded motion snapshots plus tombstones; clients retain and present both paths.
- `AnimatedModelPresentation` is covered against the real headless renderer. `dev_game`
  production-cooks and loads the authored storybook character and renders the player in third
  person.
- The base wandering-animal gameplay module produces identical authoritative, transported, and
  presented trajectories across independent headless sessions, including walk-to-idle blending.
- Typed particle prototypes now feed a bounded dense CPU pool with seeded event bursts,
  generation-safe lifetime/rate emitters, exact-world movement, deterministic overflow, age-driven
  color/size/atlas frames, and camera-facing retained instance presentation. `dev_game` wires fire
  embers, block-break puffs, and swim-entry splashes.
- The `particles` benchmark submits 50,000 active instances in one particle draw with zero drops.
  The Release reference run measured 0.495 ms median / 0.655 ms p95 simulation update, below the
  2.0 ms target; retained presentation measured 4.086 ms median / 4.234 ms p95.
- M5 verification is complete. M6 is the next delivery milestone.

## M6 — Game UI layer

### Architecture decisions

- Build a retained widget tree with stable IDs and persistent focus/drag/text state. Layout and
  paint are rebuilt each frame into `UiRenderer`; GPU resources remain renderer-owned.
- Gamepad navigation is included from the first focus implementation. Mouse-only is not accepted,
  because retrofitting focus order and modality after inventory/crafting would rewrite behavior.
- UI input is routed before action-map evaluation. Consumed pointer/key/text/gamepad input cannot
  leak to gameplay in that frame.
- Skins are data: atlas regions, nine-slice margins, fonts, colors, spacing, and state variants are
  asset/prototype records, not widget code.

### Delivery slices

1. Add geometry, constraints, row/column/grid/overlay layout, stable widget IDs, clipping, and
   invalidation.
2. Add pointer hit testing/capture, keyboard/text input, focus scopes/order, gamepad directional
   navigation, modality, accessibility labels, and input-consumption reports.
3. Add panel, label, image, button, toggle, slider, text input, scroll area, tooltip, grid slot,
   drag source, and drop target.
4. Add atlas/nine-slice skin loader and carved-wood base skin.
5. Implement inventory view-model from replicated owner inventory, optimistic drag visualization,
   authoritative `inventory.transfer_items` commands, rejection rollback, split stacks, and
   tooltips.
6. Implement HUD health/stamina/weight bindings, hotbar, pause/menu focus, and resize/DPI behavior.
7. Add UI layout/paint/input stats and inventory/HUD benchmark scenes.

### M6 verification gates

- Layout, hit-test, capture, focus, gamepad navigation, clipping, text editing, tooltip, and
  drag/drop tests are deterministic.
- Inventory moves real stacks only after authoritative acceptance and rolls back visuals on reject.
- Opening UI prevents movement/attack commands while retaining explicit allowed shortcuts.
- UI target: 2,000 widgets below `1.0 ms` p95 layout+paint, bounded allocations after warmup, and
  draw calls grouped by atlas/scissor.

### M6 progress evidence

- The retained widget tree now preserves focus, pointer capture, scroll, text-edit, and drag state
  through stable IDs. Deterministic row/column/grid/overlay layout, clipping, hit testing,
  tooltips, text input, and directional focus navigation are covered by unit tests.
- UI consumes pointer, keyboard, text, and device-neutral navigation actions before the gameplay
  action map. Pointer and keyboard adapters are live; the focus/navigation contract is ready for a
  native gamepad adapter without changing screen behavior.
- Base-mod `ui_panel` prototypes materialize carved panel, button, and slot nine-slices. Atlas and
  scissor-compatible geometry is coalesced into one renderer submission.
- The private initial player snapshot exposes only the joining player's owner inventory.
  Inventory drag/drop uses the existing authoritative transfer command with optimistic feedback,
  tracked command results, replicated-state reconciliation, rejection rollback, and right-button
  split stacks.
- The HUD binds health, stamina, carried mass, and hotbar data to replicated state. Opening the
  inventory sends neutral movement and blocks movement, jump, interaction, and drop input.
- Renderer statistics expose layout time, paint time, and widget count. The Release
  `heartstead_ui_benchmark` run built 2,000 widgets in 0.186 ms median / 0.206 ms p95, with a
  0.228 ms maximum and one draw call, below the 1.0 ms p95 target.
- M6 verification is complete.

## M7 — Actually remote multiplayer

### Architecture decisions

- Keep protocol/session logic separate from sockets. Add a true client transport endpoint rather
  than manufacturing loopback client sockets inside the server host.
- Handshake is challenge-based: protocol/content fingerprint negotiation, endpoint-bound
  anti-spoof cookie, cryptographically random session token, accept/reject, timeout, keepalive, and
  graceful disconnect. Tokens are session identity only and never save identity.
- Authoritative movement remains server-owned. Clients retain input history, predict with the same
  high-level controller, reconcile to acknowledged authoritative ticks, and replay unacknowledged
  inputs. Remote entities use an interpolation buffer with bounded extrapolation.
- Replace text-heavy live replication with bounded versioned binary codecs and per-client delta
  budgets. Text codecs remain useful for saves/tools and compatibility fixtures.

### Delivery slices

1. Add remote client socket/backend, endpoint parser/resolver, connect state machine, challenge,
   cookie/token issuance, protocol/content negotiation, timeouts, keepalive, and disconnect.
2. Integrate remote-client-only and listen/dedicated compositions in `RuntimeSession`; add CLI
   bind/connect configuration without exposing sockets to gameplay.
3. Add per-endpoint and per-session rate/token buckets, handshake amplification limits, malformed
   packet counters, bounded reassembly ownership, and session-token validation before dispatch.
4. Add movement input tick/ack fields, shared prediction state, history ring, reconciliation/replay,
   correction smoothing, and debug graphs.
5. Add remote entity interpolation buffers, jitter delay, snapshot interpolation, bounded
   extrapolation, teleport handling, and animation phase synchronization.
6. Add binary command/result/event/world-delta codecs, quantized transforms, changed-field masks,
   interest/delta budgets, priority, starvation prevention, and byte statistics.
7. Add packet/codec fuzz targets, deterministic loss/latency simulator, netem LAN scripts, and
   dedicated-server remote acceptance test.

### M7 verification gates

- Two independent processes/machines can connect, negotiate, play, disconnect, and reconnect.
- At artificial 100 ms RTT, 20 ms jitter, and 2% loss, local movement remains responsive; no
  correction exceeds the published threshold without a visible diagnostic.
- Default replication target averages below `64 KiB/s` per active client and hard-caps at
  `256 KiB/s`; deferred deltas remain fair and inspectable.
- Spoofed tokens/endpoints, replayed handshakes, malformed fragments/codecs, floods, and timeouts
  fail closed. Fuzzers and sanitizers run clean.

### M7 progress evidence

- Independent POSIX UDP client/server endpoints now negotiate a versioned binary
  hello/challenge/cookie/accept handshake. The server binds a random session token to the source
  endpoint and assigned `NetId`; protocol/content mismatches, spoofed tokens/endpoints, replayed
  cookies, and expired handshakes fail closed.
- Dedicated server `--bind` and development client `--connect` options exercise real
  remote-client-only composition. Keepalive, graceful disconnect, idle timeout, retry exhaustion,
  and reconnect-safe lifecycle are handled below gameplay.
- The local player predicts immediately through the shared controller, retains a bounded input
  history, reconciles to authoritative acknowledgements, and replays only unacknowledged inputs.
  Remote players are sampled through a six-tick jitter buffer with teleport/transition-safe
  interpolation and synchronized locomotion state.
- Commands, command results, world events, authoritative store deltas, player input bundles,
  movement snapshots, and chunk snapshot slices use bounded, versioned binary live codecs. Hot
  motion state is unreliable/latest-wins; command results, world events, and authoritative store
  deltas remain reliable.
- Per-tick transient snapshot message/byte budgets expose deferral and byte counters and rotate
  recipient priority. Per-client inbound message/byte limits, global handshake limits,
  pre-validation amplification limits, bounded fragment ownership, malformed counters, and
  session-token validation protect dispatch.
- True-socket integration covers two independent clients predicting and observing each other, as
  well as server loss and idle disconnect. The deterministic transport-codec fuzz target mutates
  seeds and submits 25,000 random inputs across packet, fragment, handshake, movement, and chunk
  codecs.
- `tools/netem_multiplayer.sh` applies or clears an explicit latency/loss profile for manual LAN
  testing. It is intentionally not invoked by automated tests because it mutates privileged host
  networking state.
- The deterministic 600-tick impaired-runtime gate runs at 100 ms RTT with ±20 ms round-trip
  jitter and 2% unreliable loss. It accepts 594/600 movement inputs, records one documented
  collision-revision bootstrap correction with a 0.825 m maximum, averages 21.0 KiB/s encoded
  server-to-client traffic, and peaks at 26.6 KiB over one second.
- A fixed one-second per-client byte window hard-caps encoded outbound traffic at 256 KiB/s.
  Reliable FIFO traffic is deferred; replaceable unreliable state is dropped and both outcomes are
  inspectable. The acceptance average remains below the 64 KiB/s target.
- M7 implementation and automated verification are complete. The privileged two-machine LAN
  walkthrough remains a release-operator check using the documented `--bind`, `--connect`, and
  netem commands rather than a containerized test.

## M8 — Production Luau

### Architecture decisions

- Add the official Luau libraries as pinned vcpkg dependencies behind `IScriptRuntime`.
- Use one VM per mod and script stage. Shared standard-library globals are sandboxed/frozen; each
  module executes in an isolated environment. Server, client, and migration cannot share mutable
  VM state.
- Compile source to bytecode at load/validation time, retain bounded diagnostics, and execute only
  through registered host APIs and data values.
- Use a custom allocator/accounting context for a hard VM memory ceiling and Luau's interrupt
  callback for instruction/deadline/cancellation enforcement.
- No filesystem, process, dynamic library, raw network, debug, or unrestricted native handles are
  opened. Host calls revalidate capability, stage, schema, and budgets at every boundary.

### Delivery slices

1. Add pinned Luau compiler/VM dependencies, backend lifecycle, bytecode compile/load, module
   environments, value marshaling, and complete error/stack cleanup.
2. Sandbox libraries/environments and register narrow native closures generated from existing host
   API descriptors.
3. Enforce per-VM memory ceiling, per-call instruction/deadline interrupt, recursion/stack/value
   limits, cancellation, GC pacing, and bounded error strings.
4. Prove server/client/migration stage isolation, deterministic module ordering, unload/reload, mod
   state persistence, and content fingerprint behavior.
5. Exercise server/client/migration host APIs with neutral test-only conformance modules. Do not
   author or port gameplay mechanics into `mods/base`; gameplay scripting begins only after the
   production boundary and all safety gates are accepted.
6. Add script timing/memory/instruction/event stats, inspection, and mod validation diagnostics.
7. Add hostile fixtures for filesystem/network access, forbidden globals, cross-mod access,
   infinite loops, recursion, memory bombs, oversized bytecode/value/event output, and malformed
   source.

### M8 verification gates

- Neutral conformance modules compile and exercise each permitted stage API without adding game
  rules or base-mod behaviors.
- Server/client/migration and mod-to-mod isolation tests pass.
- Filesystem, network, process, debug escape, infinite-loop, and memory-bomb tests all fail closed
  with typed errors while the host remains usable.
- Default limits start at 8 MiB per mod/stage VM and 100,000 VM interrupts/instruction-budget
  units per gameplay call; real content benchmarks may lower these only after telemetry.
- Disabled scripting, warnings-as-errors, sanitizers, headless server, save migration, and mod
  validator configurations remain green.

## Final integration gate

After M8, run one versioned acceptance world through:

1. deterministic generation, collision/light/fluid derivation, and save;
2. local movement, dropped resources, inventory UI, fire light/audio/particles, and day cycle;
3. dedicated-server remote join with prediction, replication, and animation;
4. mid-flow save/reload and client reconnect;
5. neutral Luau stage/capability conformance and hostile sandbox fixtures;
6. debug/release, GCC/Clang warnings-as-errors, ASan+UBSan, TSan, codec fuzzers, and published
   renderer/simulation/audio/network/script benchmark scenes.

The milestone is complete only when the acceptance report records the exact commit, dependency
baseline, build presets, benchmark machine, p50/p95/max results, and any intentionally deferred
limits.

## Primary references reviewed

- Jolt Physics 5.6 documentation and samples:
  <https://jrouwe.github.io/JoltPhysicsDocs/5.6.0/>
- Jolt `CharacterVirtual` contract and update ownership:
  <https://jrouwe.github.io/JoltPhysicsDocs/5.6.0/class_character_virtual.html>
- Jolt shape/internal-edge guidance:
  <https://jrouwe.github.io/JoltPhysicsDocs/5.6.0/index.html>
- vcpkg manifest/CMake integration:
  <https://learn.microsoft.com/vcpkg/users/buildsystems/cmake-integration>
- vcpkg manifest/versioning model:
  <https://learn.microsoft.com/vcpkg/concepts/manifest-mode>
- miniaudio engine, node graph, resource manager, and spatialization:
  <https://miniaud.io/docs/manual/index.html>
- QUIC address validation and amplification guidance:
  <https://www.rfc-editor.org/rfc/rfc9000>
- UDP usage and congestion guidance:
  <https://www.rfc-editor.org/rfc/rfc8085>
- QUIC loss detection, congestion control, and pacing:
  <https://www.rfc-editor.org/rfc/rfc9002>
- Luau embedding API:
  <https://luau.org/api/>
- Luau sandboxing, isolated environments, allocator limits, and interrupts:
  <https://luau.org/sandbox/>
