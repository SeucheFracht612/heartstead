# Game executable and application lifetime

`heartstead` is the primary player-facing executable. It starts at the main menu and keeps the
application shell alive while no world exists. `heartstead_dev_game` remains a compatibility and
diagnostic executable during migration; it is not the normal entry point.

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
| `Paused` | menu | released | pause | required | no | yes | resume or unload |
| `SessionUnloading` | application | released | loading | optional | no | no | menu or error |
| `LoadFailure` | menu | released | error | forbidden | no | no | retry or menu |
| `ConnectionFailure` | menu | released | error | forbidden | no | no | retry or menu |
| `FatalError` | application | released | error | optional | no | no | shutdown |
| `Shutdown` | application | released | none | optional while unwinding | no | no | terminal |

Each state has explicit entry, update, and exit callbacks. Entry applies cursor ownership and
builds the state's UI. Update polls loading, advances the session only when policy permits, or
performs unloading. Exit is the seam for state-specific cleanup. Platform polling, application
audio, UI construction, rendering, and presentation continue in the outer application frame while
the state machine selects which session work is legal.

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
[Game front end](game_front_end.md).

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
