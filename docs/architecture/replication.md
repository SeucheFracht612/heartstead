# Replication architecture

Replication is the engine-owned bridge from committed authoritative mutations to client
observation. World code owns the meaning of replicated records. Networking owns session identity,
message delivery, reliability, and byte budgets.

The server remains authoritative. A client command can request a mutation, but only a committed
`WorldOperation` may produce replicated world events or typed state deltas.

## Event batches

`ReplicationBatch` records one committed command outcome:

- a server-owned monotonic replication-stream sequence
- the source command sequence and source client `NetId`
- the command type
- committed world-operation events
- stable save IDs reserved by the operation

The live transport representation is the bounded, versioned
`replication.world_events.bin.v1` payload. `ReplicationTextCodec` remains available for deterministic
fixtures, tools, and compatibility reads. Failed commands, read-only commands, and mutating commands
that emit no committed events do not produce replication batches.

`ClientSession` validates sender, recipient, channel, payload type, and sequence ordering before
queuing accepted batches. Duplicate or older replication-stream sequences are rejected before world
apply code can replay them.

## Relevance filtering

`ReplicationRelevancePolicy` filters each committed batch per connected client. A rule may:

- expose only events whose subject `SaveId` is visible to that client
- allow or suppress global events whose subject ID is invalid
- filter the reserved-ID list together with the events
- fall back to broadcast behavior when no explicit client rule exists

`derive_replication_relevance_policy` builds those rules from `WorldState` simulation subjects,
viewer positions, and simulation LOD. Non-saved derived subjects such as networks and chunk regions
do not become stable saved-object visibility rules.

The resulting `WorldReplicationInterestReport` and `ReplicationRelevanceReport` keep subject counts,
per-viewer visibility, LOD exclusions, relevant clients, and filtered clients inspectable without
teaching transport code about world stores.

## Typed world deltas

World events identify what changed; typed deltas carry the authoritative records needed to observe
that change.

`plan_replication_delta` reads a committed batch against authoritative `WorldState`. It:

- aggregates repeated subject events by `SaveId`
- keeps global events separate from saved-object events
- classifies subjects by their owning store
- marks unresolved subject IDs as requiring snapshot or resynchronization fallback

`materialize_replication_delta` then copies supported records into a
`WorldReplicationDeltaSnapshot`. Current typed sections cover:

- build pieces
- persistent entities
- cargo
- inventories
- workpieces
- assemblies
- processes

Materialization reuses the existing save/runtime record shapes instead of introducing one universal
replicated-object blob. Server-only workpiece flaw bits are removed before a public record is sent.

`filter_replication_delta_snapshot` applies the same per-recipient visibility policy to the embedded
plan, events, reserved IDs, and every typed record section, then revalidates aggregate counts.

## Live and tooling codecs

`WorldReplicationDeltaSnapshotBinaryCodec` is the bounded live representation. It embeds the
sectioned save-binary snapshot and travels as the world-owned
`replication.world_delta_snapshot.bin.v1` payload over a reliable replication message.

`WorldReplicationDeltaSnapshotTextCodec` remains a deterministic tools and compatibility format. It
stores the plan header separately and embeds `SaveTextCodec` data for the materialized sections.
Both codecs reject unsupported sections, malformed payloads, sequence mismatches, and aggregate
count mismatches.

The transport bridge validates the message kind, reliable channel, payload type, embedded snapshot,
and authoritative command sequence. Decoding stays in the world layer; `HostSession` and
`ClientSession` move opaque payload bytes and remain free of build, entity, cargo, inventory,
workpiece, assembly, and process-store knowledge.

## Server delivery

`materialize_replication_deltas_for_tick` converts successful mutating command reports from one host
tick into typed deltas. Failed, read-only, and eventless commands remain explicit skipped records for
diagnostics.

`send_replication_delta_snapshots_for_tick` matches those deltas to the host tick's relevance reports
and sends complete, recipient-filtered snapshots. It skips:

- deltas that already require snapshot/resync fallback
- command sequences with no matching relevance report
- records hidden from the recipient

The returned `WorldReplicationDeltaDeliveryReport` records sent, skipped, unmatched, and
resync-skipped counts.

## Client intake and apply

`ReplicationIntake` summarizes queued event batches without mutating client world state. It reports
batch, event, and reserved-ID counts; first and last accepted sequences; monotonicity; and the split
between global and saved-subject events.

The world-layer client path is:

1. `drain_client_replication_delta_snapshots` removes only the typed world-delta envelopes from the
   protocol queue.
2. `apply_client_replication_deltas` matches decoded snapshots to queued event batches by
   authoritative command sequence.
3. `apply_replication_delta` upserts records through their concrete world stores.

Applying a delta preserves client-local runtime handles and session `NetId` values for existing
persistent entities. Build-piece and assembly changes mark room and spatial-network regions dirty so
rebuildable derived state can be regenerated. A partial delta that requires resynchronization is
rejected before client world state is mutated.

## Relationship to other replication paths

This module owns committed world events and reliable typed world-store deltas. Other live state uses
separate contracts:

- player input bundles and authoritative movement snapshots support prediction and reconciliation
- entity-motion snapshots use unreliable latest-wins delivery
- chunk bootstrap, player-centered subscription state, snapshot slices, and reliable removals use a
  dedicated identity/revision-tracked streaming path with atomic per-chunk backlog admission and a
  bounded global codec-time boundary
- transport retransmission, per-client encoded-byte ceilings, and transient snapshot budgets live in
  the networking layer

These paths share session and transport infrastructure without collapsing their different ordering,
reliability, and replacement requirements into one generic message.

## Current limits

Replication is not a universal full-state synchronization system. Unsupported or unresolved
subjects require an explicit snapshot/resync path, and chunk streaming remains separate from saved
world-store deltas. Chunk snapshots are spatially subscribed, but committed voxel event/delta
traffic still requires its own matching relevance rule. Public-Internet security, NAT traversal,
matchmaking, congestion control, and pacing are transport concerns and are not provided by this
layer.

See [Networking architecture](networking.md), [Commands](commands.md),
[World model](world_model.md), and [Runtime composition](runtime_composition.md).
