# Runtime loop, loading, pause, and recovery

The player application has one variable-rate outer frame and a fixed-rate authoritative session
clock. Native and headless applications preserve the same ownership order:

```text
poll platform events
route application/menu input
update application state
schedule fixed-step player prediction input
advance zero or more authoritative ticks
pump client transport and receive snapshots/events
reconcile and synchronize presentation
update session/application audio
build and paint UI
synchronize world rendering
render and present
```

The local input scheduler and `RuntimeSession` use the same `FixedStepConfig`; their step count and
first tick must match each frame. The local client still submits its inputs through transport and
the authoritative server. Rendering receives the clock's remainder as interpolation alpha and
never mutates authoritative state.

## Fixed-step policy

The default clock runs at 60 Hz. It accepts at most 250 ms from a frame hitch and runs at most eight
catch-up steps in one application frame. Whole excess steps are discarded and reported as
`dropped_time_us`, preventing a spiral of death. Simulation steps always use the configured fixed
delta. The accumulator retains only a sub-step remainder, so interpolation alpha stays in `[0,1)`.

Client transport is pumped even when a variable render frame produces zero fixed steps. This keeps
handshakes, keepalives, disconnects, snapshot intake, reconciliation, and presentation responsive
without inventing an authoritative tick. Dedicated and local servers share `RuntimeSession` and
therefore the same scheduler policy.

## Loading

Session construction runs on a retained asynchronous operation. The worker reports actual runtime
boundaries through `SessionStartupPhase`: request validation, content preparation, save reading,
world preparation, authoritative-server startup, client startup, transport connection,
presentation-world construction, and ready. A phase is indeterminate unless its underlying system
has a real unit count; the UI does not synthesize elapsed-time percentages.

The outer frame continues window/input polling, loading UI, audio, rendering, and cancellation.
Retrieving the future occurs only after a nonblocking readiness poll. The application retains the
future after cancellation, requests cooperative stop, and destroys any late result whose ownership
generation no longer matches.

A remote or hosted runtime is staged inside `SessionLoading` after construction and remains there
while the real transport handshake is connecting. It advances only the runtime work needed for the
live server/client composition. Handshake timeout, refusal, protocol/content failure, or explicit
cancel tears down the staged session and returns a useful ConnectionFailure screen. Local
in-memory handshakes normally finish during construction.

## Pause

Local single-player pause holds the entire runtime fixed-step clock, prediction, and authoritative
world time. The application frame, pause UI, renderer, and audio device continue. Because frame
deltas are still consumed by the outer application, resuming does not inject the wall-clock pause
duration into the simulation.

Hosted and remote multiplayer pause is a client/menu state. The runtime continues to tick or pump,
receive and reconcile state, update presentation, and maintain its transport. Gameplay input is not
submitted while the menu owns input. The pause screen explicitly warns that the world continues.
On resume, the input scheduler resets to the runtime tick so no stale input or interpolation debt is
replayed.

## Return, shutdown, and errors

Return to Main Menu saves a persistent authoritative local/hosted world first. A save failure keeps
the pause screen usable and exposes the error; it does not destroy the only live copy. A successful
save transitions to SessionUnloading, closes command intake, shuts down the session in the documented
teardown order, refreshes save discovery, and returns to MainMenu.

Application shutdown during a persistent session attempts the same authoritative save before
teardown. Shutdown during loading requests cancellation, retains and joins the operation, and
destroys a late runtime before application services. Process exit is never the cleanup strategy.

Save/world startup failures use LoadFailure. Handshake, transport, and live multiplayer failures use
ConnectionFailure. Runtime simulation faults and session-scoped presentation synchronization errors
destroy the session and recover to the appropriate error screen. Device-wide renderer or platform
failures remain fatal because the menu cannot render safely without those application services.

Every successful application transition is logged with source, destination, sequence order, and
reason; error transitions include their diagnostic code/message. Fixed-step dropped time is logged
with the session ownership generation. The same values feed the runtime diagnostics view.
