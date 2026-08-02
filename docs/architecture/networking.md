# Networking architecture

Networking is an engine-owned boundary below server-authoritative commands. It transports bounded
messages and session identity; it does not decide what a command means or mutate gameplay stores.

## Layering

```text
game command / replication semantics
        |
client session and host session protocols
        |
transport messages, reliability, fragmentation, maintenance
        |
in-memory transport or POSIX UDP backend
```

`ServerCommandDispatcher` validates meaning and performs transactions. The transport treats command
and replication payloads as opaque versioned bytes.

## Transport-neutral contract

`ITransportHost` exposes server identity, connected clients, send/drain operations, capabilities,
statistics, and deterministic maintenance polling. A transport message carries:

- message kind and payload type;
- reliable or unreliable channel;
- sequence and timestamp;
- bounded opaque payload bytes;
- sender and recipient session `NetId` values in its envelope.

`NetId` is session identity. It must never become a persistent save ID, player UUID, entity save ID,
or world-object identity. Numeric endpoints also stay outside gameplay identity.

The live path uses bounded, versioned binary codecs for commands, command results, events, world
deltas, input bundles, movement snapshots, and chunk snapshot slices. Deterministic text codecs are
retained where useful for fixtures, tools, and compatibility inspection.

## Backends

### In-memory

The in-memory host is used by local server/client compositions and deterministic tests. It assigns
session IDs, validates payload/client limits, preserves reliable command order, and can apply seeded
latency, jitter, and unreliable loss without depending on wall-clock scheduling.

### POSIX UDP

The external backend is project-owned POSIX UDP despite the historical internal backend name
`external_library`. It provides independent server and client implementations. The server binds a
numeric IPv4 endpoint; the client binds an ephemeral local endpoint and connects to a numeric IPv4
server endpoint.

The current native socket path is POSIX-specific. Higher layers depend on the transport interface so
a proven transport library or another platform backend can replace or supplement it without moving
command semantics downward.

## Admission and session lifecycle

An unknown UDP endpoint is admitted through a bounded hello/challenge/response/accept exchange:

1. the server returns an endpoint-bound, expiring challenge cookie without allocating a gameplay
   session;
2. the client proves receipt and supplies protocol/content compatibility data;
3. the server validates the cookie and compatibility before assigning a client `NetId`;
4. the accepted session receives a cryptographically random token;
5. established datagrams are accepted only when claimed identity, source endpoint, and token match.

The backend limits pre-validation amplification and handshake response rate. Incomplete handshakes
and idle sessions time out. Established peers exchange keepalives and can close gracefully with a
bounded disconnect reason.

The token mitigates off-path datagram injection. It is not encryption, account authentication, or
forward secrecy.

## Reliability and fragmentation

Already-encoded transport packets can be split into bounded indexed fragments. Reassembly accepts
out-of-order and exact duplicate fragments, rejects conflicting data or impossible metadata, caps
memory/ownership, and publishes a packet only when its complete byte count matches the declaration.

Reliable messages use transport-level acknowledgements, duplicate suppression, retry scheduling,
and a bounded maximum-attempt drop. Sequence `0` is reserved for eligible transport control
messages; reliable gameplay streams use nonzero ordered sequences. A duplicate reliable delivery
is acknowledged again but not delivered twice.

This is a bounded reliability primitive, not a full congestion-control protocol. Unreliable
latest-wins state may be replaced or dropped under pressure; reliable FIFO traffic is deferred
within explicit backlog and byte budgets.

## Authority and command flow

A client session creates commands with its accepted client ID and monotonically increasing local
sequence. The host session validates transport/session shape, reconstructs the command envelope,
and dispatches it through the authoritative server.

The server validates permissions and command-specific preconditions, commits or rolls back the
transaction, and returns a reliable command result. A result identifies the command sequence/type,
success or error, whether world mutation committed, and relevant event counts without exposing
server-only rollback diagnostics.

Reliable client command replay or out-of-order delivery is rejected before authoritative execution.
Transport success never implies command success.

## Replication

The server owns a monotonic replication stream sequence independent of every client's command
sequence. Committed world events and typed deltas are filtered per recipient using world-derived
interest/relevance data before serialization and send.

- world events and authoritative store deltas use reliable delivery;
- movement and entity-motion snapshots use unreliable latest-wins delivery;
- player input bundles include recent-frame redundancy on the unreliable path;
- transient snapshot work is bounded by global and per-client message, payload-byte, and measured
  serialization-time budgets;
- client intake rejects duplicate or older replication batches before applying state.

The transport does not know about chunks, entities, inventories, processes, or assemblies. World
code materializes and applies typed snapshots around the protocol boundary. See
[Replication](replication.md).

### Transient tick admission

Movement and entity-motion codecs produce one immutable payload per source state, then reuse that
payload for every admitted recipient. Codec time is therefore paid once globally instead of once
per source/client pair. Every recipient that participated in the codec operation is conservatively
attributed the complete measured source-codec cost, even if byte/message admission later rejects its
candidate. The global statistic records only real elapsed codec work. The two values are
deliberately distinct: attributed client time is an isolation charge, not a second claim about
elapsed CPU time.

The default 60 Hz policy admits at most 512 messages, 256 KiB of encoded payload, and 4,000 us of
codec work globally per tick. A client may consume at most 128 messages, 64 KiB, and 2,000 us of
attributed codec work in that tick. These are configurable safety defaults, not calibrated
multiplayer SLOs. Stable identity ordering plus rotating recipient, source, and snapshot-class
cursors makes degradation deterministic while preventing one client, low identity, or traffic
class from owning every constrained tick.

Payload-byte and message limits are strict. A codec call is non-preemptible: it may start only while
the global and at least one recipient time budget remain, and the one call that crosses either
boundary may finish and distribute its already-produced payload. Actual/attributed overshoot,
maximum single-call time, per-limit deferrals, and per-client totals remain inspectable. Snapshot
tombstones stay outside this replaceable-state budget because dropping a removal indefinitely is a
correctness failure. Reliable command results, world deltas, and chunk snapshots also remain outside
it and require their own bounded-backlog policy.

The shared-payload structure follows the CPU-scaling rationale of Unreal's persistent replication
lists and per-connection prioritization in the official
[Replication Graph documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph-in-unreal-engine).
Per-client isolation is consistent with the flow-isolation motivation of
[RFC 8290 (FQ-CoDel)](https://www.rfc-editor.org/rfc/rfc8290), without claiming that this
application scheduler implements FQ-CoDel. The tick controller complements rather than replaces
transport congestion control, pacing, and bytes-in-flight accounting described by
[RFC 9002](https://www.rfc-editor.org/rfc/rfc9002).

## Prediction and interpolation

The remote client uses the same movement controller as the authoritative server, retaining a bounded
input history for prediction and reconciliation. Authoritative movement snapshots acknowledge
input progress and may trigger replay or a visible hard correction when convergence is impossible.
Remote entities are presented through delayed interpolation rather than by treating arrival time as
simulation time.

Prediction is presentation state. It cannot commit terrain, inventory, entity, process, or other
world mutations.

## Budgets and observability

The host enforces a hard per-client encoded-wire traffic ceiling of 256 KiB over one second and
targets an average server-to-client rate below 64 KiB/s for the maintained impaired-runtime
profile. The transient controller separately bounds application payload and codec work per tick;
neither substitutes for a complete congestion controller. Replaceable unreliable state can be
dropped. Per-client inbound message/byte limits, global handshake limits, fragment/reassembly
bounds, timeout/retry bounds, and pre-validation amplification limits protect the authoritative
loop. Reliable outbound queues are retained for correctness but do not yet have their final
application backlog caps.

Statistics expose encoded bytes/messages, backlog, drops, retransmits, malformed/rate-limited
traffic, reassembly ownership, command/replication counts, prediction corrections, and related
maintenance activity. Exact measurements from a single acceptance run belong in test artifacts or
benchmark records, not this architecture contract.

## Security and deployment boundary

The current remote path is for controlled LAN/testing environments. It does **not** provide:

- payload or metadata encryption;
- account authentication or trusted player identity;
- forward secrecy;
- NAT traversal or relay;
- DNS-based discovery or matchmaking;
- production denial-of-service protection;
- complete congestion control and pacing.

Do not expose it to untrusted Internet clients. Public deployment requires these properties or a
migration to a proven secure transport while retaining the command, replication, and persistence
boundaries above it.

## Running and verification

Use `heartstead_dedicated_server --bind ADDRESS:PORT` and
`heartstead --connect ADDRESS:PORT` for the socket-backed composition. The standalone
server currently owns no persistent save lifecycle. See [Running Heartstead](../dev/running.md) and
[Testing](../dev/testing.md) for exact commands and impaired-network testing.
