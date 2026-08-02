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
- expose chunk-scoped events only when that exact chunk is visible to the client
- filter the reserved-ID list together with the events
- fall back to broadcast behavior when no explicit client rule exists

`OperationEvent::routing_chunk` is owner-side routing metadata, not part of the event payload.
The authoritative host consumes it before encoding `replication.world_events.bin.v1`; clients do
not need to receive a relevance key that has already served its purpose. An invalid-subject event
with a routing chunk is spatial rather than global: chunk interest decides visibility. Unscoped
invalid-subject events continue to use the global-event rule, while saved-subject events retain the
existing subject and private-access checks.

`derive_replication_relevance_policy` builds those rules from `WorldState` simulation subjects,
viewer positions, and simulation LOD. Non-saved derived subjects such as networks and chunk regions
do not become stable saved-object visibility rules.

The resulting `WorldReplicationInterestReport` and `ReplicationRelevanceReport` keep subject counts,
per-viewer visibility, LOD exclusions, relevant clients, filtered clients, spatial event-delivery
counts, and filtered logical payload bytes inspectable without teaching transport code about world
stores.

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

`filter_replication_delta_snapshot` applies the same per-event spatial and per-recipient subject
visibility policy to the embedded plan, events, reserved IDs, and every typed record section, then
revalidates aggregate counts.

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

`ServerRuntime` consumes that report before ordinary chunk-subscription synchronization. Delivery
and authoritative command rows are matched in linear source order with the same normalized
replication-sequence rule used on the wire. For each recipient that actually received a routed voxel
event, a complete publication may advance only when chunk identity matches and its revision is
exactly one behind the event. If the authoritative chunk is now at that revision, this suppresses
the redundant full 32-slice snapshot. Missing, partial, identity-mismatched, or non-contiguous
publications remain stale so the ordinary full-snapshot path repairs them. Advanced, avoided, and
gap counts are retained in tick inspection and Tracy plots.

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

For voxel events, `ClientRuntime` requires an authoritative remote chunk base and rejects generation
or revision gaps. A contiguous accepted edit advances the remembered remote revision, allowing the
next command to use the delta-updated publication without waiting for a complete snapshot. A newer
complete snapshot may supersede an older observed event only when the persistent cell already
matches that event; this tolerance does not yet provide a unified ordering policy for every mixed
snapshot/delta queue shape.

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

`ServerRuntime` derives each live chunk-interest rule from complete `ChunkPublication` records whose
load identity and content revision exactly matched the authoritative chunk before command
execution. A newly subscribed client therefore does not receive a delta before its base snapshot,
while a client holding the preceding revision may receive the next edit. Both the immediate event
batch and typed delta use the same host-tick relevance decision. Omitted replication sequence values
are legal; when a filtered client later enters that chunk, the ordinary identity/revision snapshot
path installs the complete current state.

## Current limits

Replication is not a universal full-state synchronization system. Unsupported or unresolved
subjects require an explicit snapshot/resync path, and chunk streaming remains separate from saved
world-store deltas. Voxel event/delta traffic is spatially filtered and isolated material hot-edit
throughput is calibrated. Revision-safe intake when an older voxel delta and a newer full snapshot
are both queued, impaired-network behavior, and long-soak queue/memory stability remain open.
Public-Internet security, NAT traversal, matchmaking, congestion control, and pacing are transport
concerns and are not provided by this layer.

See [Networking architecture](networking.md), [Commands](commands.md),
[World model](world_model.md), and [Runtime composition](runtime_composition.md).
