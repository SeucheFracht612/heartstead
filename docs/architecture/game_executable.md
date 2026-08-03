# Game executable and application lifetime

`heartstead` is the primary player-facing executable. It starts at the main menu and keeps the
application shell alive while no world exists. `heartstead_dev_game` remains a standalone
presentation diagnostic for focused renderer/VFX inspection; it is not a normal gameplay entry
point and is not required for move/render/edit testing.

## Ownership

`GameApplication` owns services whose lifetime normally matches the process:

- platform backend and window;
- renderer, render device, UI renderer, and the renderer's asset-facing resources;
- audio device installed by the Heartstead application mode;
- application job system;
- input snapshots supplied by the platform;
- the validated content view and cooked asset store retained by the player executable;
- process-entry logging, exception, and fatal-error handling.

`HeartsteadApplicationMode` owns the player shell, retained menu widgets, application state
machine, content-backed audio factory, loading operation, and an optional `GameRuntime`. A runtime
session is absent in the menu and exists only between successful loading and completed unloading.
World state is never stored in `GameApplication`.

The application services are destroyed after the mode shuts down. A session is destroyed before
application audio, renderer, window, jobs, and platform services. Process exit is not used as the
session cleanup mechanism.

## State machine

All state changes go through `ApplicationStateMachine::transition`. A transition records its
source, destination, sequence, reason, and optional error. Invalid transitions and error states
without an error payload are rejected before lifecycle callbacks run.

| State | Input | Cursor | UI | Session | Server ticks | World render | Recovery |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `Boot` | application | released | boot | forbidden | no | no | fatal or shutdown |
| `MainMenu` | menu | released | menu | forbidden | no | no | launch or shutdown |
| `SessionLoading` | menu | released | loading | optional | no | no | cancel or error |
| `InGame` | session | captured | session | required | yes | yes | pause, unload, error |
| `Paused` | menu | released | pause | required | local: no; multiplayer: yes | yes | resume or unload |
| `SessionUnloading` | application | released | loading | optional | no | no | menu or error |
| `LoadFailure` | menu | released | error | forbidden | no | no | retry or menu |
| `ConnectionFailure` | menu | released | error | forbidden | no | no | retry or menu |
| `FatalError` | application | released | error | optional | no | no | shutdown |
| `Shutdown` | application | released | none | optional while unwinding | no | no | terminal |

Each state has explicit entry, update, and exit callbacks. Entry applies cursor ownership and
builds the state's UI. Update polls loading, advances the session only when policy permits, or
performs unloading. Exit is the seam for state-specific cleanup. Platform polling, application
audio, UI construction, rendering, and presentation continue in the outer application frame while
the state machine selects which session work is legal. The Paused row is mode-dependent: a local
single-player runtime is held, while hosted and remote multiplayer runtimes continue pumping so a
client-side menu cannot stop a live server.

## Front end and loading behavior

The root menu exposes Continue, New World, Load World, Multiplayer, Developer Worlds, Options, and
Quit. Continue is disabled with an explanation when no compatible recent save exists. Menu
navigation is modeled separately from rendering, including the delete-confirmation parent, so
focus/back transition behavior is regression tested.

Every session action creates a `SessionLaunchRequest` and starts `GameRuntime` construction on a
background operation.
The foreground continues polling window/input events, updating audio, routing the retained widget
tree, painting the loading screen, and presenting frames. Completion is observed without blocking
a frame and then applied through the transition API. Cancellation requests cooperative stop,
retains the outstanding operation until completion, and unloads any late result. Each operation
and session carries a monotonically increasing ownership generation, so a stale result cannot
replace the current session.

Local worlds use the normal authoritative composition:

```text
heartstead
  GameApplication (window/renderer/audio/jobs)
  HeartsteadApplicationMode
    GameRuntime
      RuntimeSession
        ServerRuntime (authoritative)
        in-memory transport
        ClientRuntime (prediction/replication/presentation world)
```

The menu does not have a `WorldState`. Renderer cleanup is registered with the session and runs as
part of its explicit reverse-order teardown before the runtime is destroyed. The full teardown and
launch descriptor are documented in [Runtime composition](runtime_composition.md). Save discovery,
world operations, menu capabilities, and application settings are documented in
[Game front end](game_front_end.md). Frame/tick ordering, measured loading phases, and pause/error
behavior are documented in [Runtime loop and transitions](runtime_loop.md).

## Build and launch

From a configured default debug tree:

```bash
cmake --build --preset default-debug --target heartstead
./build/default-debug/apps/heartstead/heartstead
```

For a windowless application-shell smoke test:

```bash
./build/default-debug/apps/heartstead/heartstead --frames 120
```

`--frames` implies headless mode. `--native-frames N` bounds a real windowed run.

## Command-line launches

Command-line launches enter `MainMenu` first and invoke the same launch helpers as the front end.
Exactly one automatic session selector may be supplied:

```bash
heartstead --scenario base:scenarios/renderer_proof
heartstead --world homestead
heartstead --world /absolute/path/to/save
heartstead --new-world "CLI Homestead" --seed 1337
heartstead --connect 127.0.0.1:7777
heartstead --host homestead
heartstead --safe-mode
```

`--safe-mode` selects the low renderer preset and marks created requests with the `safe-mode`
runtime option. It does not bypass content validation, server authority, save compatibility, or
the session state machine. `--help` and `--version` are supported. Conflicting selectors and seeds
attached to load/connect/host requests fail before application mutation.

## Runtime diagnostics

`F3` toggles the application diagnostics panel. It reports application/session/connection state,
world and save identity, ownership generation, authoritative and fixed ticks, interpolation and
dropped time, application plus session jobs, loading operations, connections, registered cleanup
callbacks, entity/physics/presentation/render/audio/asset counts, process RSS/thread/open-file
counts on Linux, and renderer memory. Live RSS is the cheap approximate `/proc/self/statm` sample
and is labeled as such; precise `/proc/self/smaps_rollup` PSS/private-resident values appear only
when an explicit precise sample is available. Vulkan device usage/budget is shown only when
`VK_EXT_memory_budget` telemetry is valid; otherwise the UI says it is unavailable.

`F7` toggles the compact performance overlay. It reports sampled FPS, renderer CPU-frame time,
whole-process CPU use normalized across logical processors, timestamped GPU-frame time and derived
GPU load, approximate process RSS under the compact `WORLD RAM` label, resident terrain/static/far
mesh bytes, and resident/visible/conservatively culled chunk counts. CPU/RSS refresh at 4 Hz. GPU
fields report `N/A` until timestamp data is valid. The compact `OCCLUDED` label currently represents
the renderer's combined distance/frustum-culled chunk count; it is not a distinct chunk-HZB query.
Panel dimensions, padding, and text follow the effective application UI scale.

## Extension rules

Add a menu screen to `MainMenuScreen` and `MainMenuNavigation`, then build it through the retained
widget tree and route its action to `HeartsteadApplicationMode::launch`. Do not construct a runtime
from a widget callback. Add a session mode by extending `SessionMode`, its launch validation and
central topology resolution in `GameRuntime::start_session`; then add teardown counters and a
replacement test before exposing it through the menu or CLI.
