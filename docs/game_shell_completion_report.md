# Heartstead game shell completion report

This report describes the implemented repository state on 2026-08-01. It distinguishes verified
behavior from remaining limitations.

## 1. Final application architecture

`apps/heartstead` is the primary process. `GameApplication` owns platform/window, renderer and UI
renderer, application jobs, installed audio, frame polling, rendering, and shutdown. Its
`HeartsteadApplicationMode` owns settings, menus, the application state machine, save/scenario
catalogs, an asynchronous launch operation, and at most one `GameRuntime`. World state never
belongs to the application shell.

## 2. Application state machine

The explicit states are Boot, MainMenu, SessionLoading, InGame, Paused, SessionUnloading,
LoadFailure, ConnectionFailure, FatalError, and Shutdown. A checked transition table rejects
invalid edges and error states without diagnostic payloads. Each state has entry/update/exit hooks
and a policy for input, cursor, UI, session presence, simulation, rendering, events, and audio.

## 3. Runtime-session ownership

`RuntimeSession` owns the server/client topology, transport, fixed-step clock, presentation world,
world/physics through the server, and registered application-backed session resources. Every
session carries a generation, rejects commands after stop begins, rejects duplicate cleanup names,
and produces a teardown report with entity, physics, presentation, job, callback, transport,
client, and server barriers.

## 4. Local single-player composition

Local play is always `ServerRuntime` plus `ClientRuntime` over in-memory transport. Input is sent
through the normal player command/bundle route. Authoritative ticks produce the same snapshots and
events consumed by prediction, reconciliation, presentation, models, audio, and rendering. Voxel
editing uses authoritative remove/place commands; presentation never mutates the server world.

## 5. Remote-client composition

Remote play owns a client, POSIX UDP transport, handshake/connection state, timeouts, disconnect,
and error recovery. The same application remains responsive while connecting and may cancel into
session unloading. The dedicated server remains a separate executable.

## 6. Main-menu capabilities

The retained production UI exposes Continue, New World, Load World, Multiplayer, Developer Worlds,
Options, and Quit. Mouse/keyboard focus, confirmation, back, disabled reasons, resize-aware layout,
cursor capture/release, UI scale, pause, failure acknowledgement, and return to menu are routed by
the application state. The controller toggle persists, but the platform input layer has no gamepad
event backend yet.

## 7. Save and world management

Continue selects the newest compatible recent slot. New World validates name/seed, creates a
persistent authoritative session, and publishes an initial save. Load World reports metadata,
compatibility/corruption, path, placeholder thumbnail, versions, required mods, and migration
status. Slots can be loaded, hosted, renamed, duplicated, deleted after confirmation, refreshed,
and copied to the clipboard. Explicit CLI save directories are supported outside the catalog.

## 8. Developer-world system

Namespaced scenario prototypes provide stable ID, browser text/category/tags, source, seed,
fixture/setup hook, spawn/orientation, initial state, requirements, policy, and benchmark/debug
metadata. The registry discovers entries without menu edits. The generated Foundation Slice and
packaged far-coordinate Renderer Proof World both launch through normal sessions. Fixture worlds
do not receive the foundation chunks at the origin, and their physics island is anchored at the
large-coordinate spawn.

## 9. Tick and frame-loop behavior

The outer loop polls events and input, updates the state, updates audio/UI, prepares presentation,
renders, and presents at variable rate. `RuntimeSession` owns a 60 Hz fixed-step clock with a
250 ms frame clamp and eight-step catch-up limit. Client transport and presentation pump even on
zero-tick render frames. Prediction inputs share the same tick schedule and mismatches fail with a
diagnostic. Dropped time is logged and shown on F3.

## 10. Loading and pause behavior

Session construction runs asynchronously and reports real phases: validation, content, save read,
world preparation, server, client, transport, presentation, ready, or cancelling. The main loop
continues events/UI/render/audio while polling completion. Local pause freezes server ticks and
prediction while the menu runs. Hosted/remote pause keeps the live runtime pumping and warns that
the world continues.

## 11. Session teardown sequence

Teardown closes the command gate, disconnects transport, stops authoritative ticking, runs
session cleanup in reverse order (model/audio before renderer clear), clears presentation, destroys
client/transport/server, cancels server-owned collision jobs, destroys physics/world state, clears
input/camera/runtime diagnostics, and returns to MainMenu. Late load results are retained until
completion and discarded unless their generation is still current.

## 12. Command-line options

The supported player commands are `heartstead`, `--scenario ID`, `--world PATH_OR_ID`,
`--new-world NAME`, `--seed N`, `--connect HOST:PORT`, `--host PATH_OR_ID`, `--safe-mode`,
`--headless`, `--frames N`, `--native-frames N`, `--help`, and `--version`. Automatic launches
first enter MainMenu and then call the same launch helpers as UI actions.

## 13. Tests and stress-test results

Focused coverage includes state policies/transitions and invalid edges; menu navigation, settings,
save management, CLI parsing, and diagnostics; generated/fixture discovery and launch; startup
phases; local create/stop/replacement; stale generations; duplicate cleanup rejection; remote
timeout/cancellation and local recovery; renderer session clearing; headless shutdown while menu,
loading, and active; and a repeated game-shell lifecycle test.

The lifecycle test performs one warm-up plus 17 replacements across generated, packaged
large-coordinate, and saved/reloaded persistent worlds. It runs authoritative ticks, saves,
registers session callbacks, and asserts zero surviving session jobs, presentation objects,
entities, and physics bodies after every teardown. The same test passed a Clang 18
AddressSanitizer/UndefinedBehaviorSanitizer build with LeakSanitizer enabled.

The final warning-as-error build completed for every target, and all 141 registered CTest cases
passed (including 44 executable smoke tests). A three-frame native player launch also reached
MainMenu and shut down cleanly on the verification host.

## 14. Memory and resource-cleanup results

The verified warning-as-error headless run reported RSS 20,926,464 → 24,961,024 bytes after
warm-up and 17 replacements (about 3.9 MiB retained), threads 1 → 1, and open files 5 → 5. The test
permits a conservative 128 MiB allocator/cache bound and fails on accumulating threads/files.
Renderer tests verify session clear resets retained chunk/object/skin counts. Vulkan device
usage/budget telemetry exists in F3 when supported, but this environment did not run a repeated
native Vulkan GPU-memory measurement; no GPU-memory stability number is claimed.

ASan process RSS is deliberately excluded from the native 128 MiB assertion because sanitizer
shadow memory and quarantine are not application retention. Its run still checks all teardown,
thread, and file counters and lets LeakSanitizer decide heap reachability.

## 15. Important changed files and directories

- `apps/heartstead/`: primary executable and CLI entry.
- `game/application/`: application state, settings, front end, launch parser, diagnostics, and mode.
- `game/runtime/`: launch descriptor, topology, ticking, ownership generations, and teardown.
- `game/scenarios/` and `mods/base/data/scenarios/`: registry, shared setup, and developer worlds.
- `engine/save/`: slot discovery and world management.
- `engine/physics/`: large-coordinate fixture collision diagnostics and physics-island use.
- `tests/`: application, front-end, scenario, lifecycle, renderer cleanup, smoke, and stress tests.
- `docs/architecture/` and `docs/dev/`: maintained ownership, launch, authoring, run, and test docs.

## 16. Build and launch instructions

```bash
export VCPKG_ROOT=/absolute/path/to/vcpkg
cmake --preset default-debug
cmake --build --preset default-debug --target heartstead
./build/default-debug/apps/heartstead/heartstead
```

Use `ctest --preset default-debug` for the full configured suite and
`ctest --test-dir build/default-debug -L lifecycle --output-on-failure` for lifecycle checks.
ASan/UBSan and leak-detection presets are documented in `docs/dev/build_instructions.md`.

## 17. Known limitations

- Initial content validation and cooked terrain-asset bootstrap still occur before the native
  window/event loop exists; session/save/scenario/network loading is asynchronous, but boot is not.
- The complete native path is Linux/X11/Vulkan. Controller events, wider platform backends, DNS,
  public discovery, accounts, encryption, NAT traversal, and production Internet security are not
  implemented.
- The main shell currently integrates terrain, models, movement, voxel edits, camera, world audio,
  menu/pause UI, and diagnostics. The broader environment/VFX/inventory/map probes remain in the
  standalone diagnostic until promoted as actual gameplay UI.
- Thumbnails are placeholders; opening a save location copies its path rather than invoking an OS
  file manager. Save migrations are reported but never automatic.
- Safe mode selects conservative rendering and tags the runtime request; it is not a general
  mod-bisect or content-disable system.
- Dedicated-server persistence and replay sessions are not implemented. Direct-address transport
  remains controlled-LAN/testing technology.
- Automated resource tests measure headless CPU/process ownership. A native repeated-world GPU
  capture must still be run on a Vulkan host before claiming a GPU-memory bound.

## 18. Recommended first gameplay milestone

Build a small server-authoritative settlement loop as one gameplay module: gather two existing
resource types, place one existing build-piece prototype, run one deterministic production recipe,
persist the result, and replicate its presentation/UI. Exercise it in a new data-authored scenario
and in a persistent save, then extend the lifecycle stress sequence with that scenario. This tests
the new shell at every important boundary without introducing another application architecture.
