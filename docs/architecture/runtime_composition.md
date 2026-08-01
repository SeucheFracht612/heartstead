# Runtime composition and gameplay boundary

Heartstead uses the same server-authoritative command and replication model in local, remote, and
test compositions. Applications choose which roles and presentation backends exist; they do not
redefine gameplay semantics.

## Supported compositions

| Composition | Roles | Transport | Presentation | Persistence |
| --- | --- | --- | --- | --- |
| Player-facing Heartstead application | menu or server + client | in-memory | native X11/Vulkan | temporary developer world in the first shell stage |
| Interactive local development game | server + client | in-memory | native X11/Vulkan | default or explicit local save |
| Bounded/headless development run | server + client | in-memory | headless or bounded native | only with explicit `--save` |
| Remote development client | client only | POSIX UDP to numeric IPv4 | native or bounded/headless | none; server owns truth |
| Dedicated server | server only | POSIX UDP bind | headless | memory-only in the current executable |
| Tests and samples | selected roles | in-memory or socket-backed | usually headless | fixture/explicit only |

The same runtime library can be configured programmatically for focused tests, but shipped
applications should use one of the explicit ownership shapes above.

## Lifecycle and ownership

The player application owns the top-level `GameApplication` across menu and world transitions. A
session is optional and explicitly unloaded before returning to the menu. Dedicated servers and
focused tools own their corresponding top-level loop and construct only the roles they need. The
server role owns authoritative `WorldState`, command dispatch, simulation ticks,
replication, and any configured save lifecycle. The client role owns input collection, protocol
state, predicted presentation state, and renderer/audio/UI extraction.

Subsystem shutdown occurs in reverse ownership order. Native presentation must finish in-flight
backend work before releasing dependent resources. Network sessions receive a graceful disconnect
where possible, then queues and runtime stores are destroyed by their owners.

No process-global world, script VM, renderer, session, or save database is required for normal
operation.

## Local frame path

A local interactive frame follows the same conceptual boundary as remote play:

```text
platform input
  -> client input/actions
  -> in-memory transport command/input bundle
  -> authoritative server tick and transactions
  -> command result + replication/state snapshots
  -> client intake/prediction reconciliation
  -> presentation extraction
  -> renderer, audio, UI, particles
```

Sharing a process is an optimization and development convenience. Client code must not reach into
server stores to make gameplay changes.

## Remote frame path

Remote play replaces only the transport and process boundary:

```text
client process                         server process
input/prediction                       UDP intake/session validation
  -> encoded bundle/datagram   --->      -> authoritative tick
  <- results/snapshots/events   <---      -> recipient-filtered replication
reconcile/interpolate                   maintenance/timeout/rate limits
  -> presentation
```

Command meaning, transaction rules, event types, stable IDs, and world codecs stay above the
transport. See [Networking](networking.md) and [Replication](replication.md).

## Commands, events, and entities

Input, UI, scripts, admin tools, and network clients express meaningful changes as commands or
bounded host events. The authoritative dispatcher validates identity, permission, reach, ownership,
prototype/schema, expected revisions, and command-specific preconditions before mutation.

Transactions either commit all intended store changes and emit deterministic events, or roll back.
Entity runtime handles are process-local; save IDs and prototype IDs carry durable identity.
Replication materializes recipient-appropriate events and typed state deltas without teaching the
transport about world stores.

## Client and presentation ownership

The client may own:

- input mapping and action state;
- a bounded history for predicted movement;
- interpolated remote presentation state;
- decoded client-visible replicas and UI models;
- camera, renderer, audio, animation, particle, and UI resources;
- diagnostics and developer overlays.

The client does not own authoritative inventory, terrain, entities, processes, permissions,
profiles, discovery, or save state. Prediction may improve responsiveness but never grants the
ability to commit a world change.

Headless mode replaces presentation backends with deterministic validators or omits presentation
work while preserving server/client protocol behavior required by the test.

## Gameplay module extension

`engine/` defines reusable mechanisms and contracts. `game/` composes those mechanisms and owns
Heartstead-specific systems. `mods/base` defines base prototypes, assets, recipes, scripts, and
other content through public pipelines.

When adding a gameplay feature:

1. put generic storage/validation/scheduling capability in the engine only when more than one game
   feature can use it without Heartstead-specific meaning;
2. put orchestration and game rules in `game/`;
3. put authored values and content identity in mods;
4. route mutations through commands/transactions;
5. expose presentation through extracted data and stable engine-facing descriptors;
6. version persistent or network data before incompatibly changing it.

Avoid adding a new top-level runtime or bypass path for one feature. Extend the existing server,
client, command, snapshot, and presentation owners.

## Persistence

A normal unbounded local native `heartstead_dev_game` run opens
`saves/foundation_slice_0_1` unless overridden. Bounded/headless runs persist only with an explicit
`--save DIRECTORY`; `--no-save` disables persistence. A remote client cannot use `--save` because
it has no authoritative world.

The current `heartstead_dedicated_server` accepts `--bind` and optional `--ticks`, but does not
construct a save database or expose a save option. Its world is lost when the process exits. This
is an implementation limit, not permission for a client to save the server's state.

Save ownership should be added to the dedicated composition by reusing the same versioned snapshot,
database, migration, log, and shutdown boundaries—not by creating a second server format.

## Headless and bounded execution

Headless and bounded modes exist for tests, CI, tools, replay, diagnostics, and deterministic
integration checks. They must not silently change command authority or data formats. A bounded run
reports failures and exits; it should not leave background threads or pending publication work.

## Failure behavior

- Invalid command-line combinations fail before runtime mutation.
- Failed subsystem initialization unwinds already-created owners in reverse order.
- Command validation failure returns a result and leaves authoritative state unchanged.
- Stale asynchronous results are rejected by revision/generation checks.
- Transport timeout or retry exhaustion disconnects the affected session rather than stalling the
  authoritative loop.
- Save publication failure retains the previous committed state and surfaces an error.
- Presentation failure does not grant the client authority or corrupt durable state.

Operational commands and controls are documented in [Running Heartstead](../dev/running.md).
