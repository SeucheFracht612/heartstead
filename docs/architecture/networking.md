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
it and enter the separate reliable application backlog below.

The shared-payload structure follows the CPU-scaling rationale of Unreal's persistent replication
lists and per-connection prioritization in the official
[Replication Graph documentation](https://dev.epicgames.com/documentation/en-us/unreal-engine/replication-graph-in-unreal-engine).
Per-client isolation is consistent with the flow-isolation motivation of
[RFC 8290 (FQ-CoDel)](https://www.rfc-editor.org/rfc/rfc8290), without claiming that this
application scheduler implements FQ-CoDel. The tick controller complements rather than replaces
transport congestion control, pacing, and bytes-in-flight accounting described by
[RFC 9002](https://www.rfc-editor.org/rfc/rfc9002).

### Reliable application backlog

Every reliable command result, world delta, chunk slice, bootstrap record, and tombstone enters a
host-owned per-client FIFO before the transport sees it. Exact encoded-wire size is computed once at
admission and retained with the entry, so retries and budget checks do not repeat the sizing encode.
Producers that require an indivisible logical publication can submit a reliable replication batch:
the host validates every message and exact wire size, rejects the whole batch at a count/byte
boundary, or appends every message contiguously. Chunk snapshots use this path, so a 32-slice
snapshot never becomes a half-admitted backlog entry when capacity changes.
The default hard backlog envelope is 8,192 messages/64 MiB globally and 1,024 messages/8 MiB per
client. Direct producers receive an explicit admission error when either boundary is full. If the
host command gateway has already committed and its mandatory result or immediate event replication
cannot be admitted, the host disconnects only the affected client and reports that overload; it
never keeps the client active after silently losing that output. Later replication producers still
receive an explicit failure and must choose disconnect, resync, or retry at their ownership boundary.
A single encoded message that cannot fit the configured one-second and per-tick byte budgets is also
rejected at admission, preventing a permanently undrainable queue head.

Reliable sends drain in rotating round-robin order under strict tick ceilings:

| Metric | Global/tick | Per client/tick |
| --- | ---: | ---: |
| Messages | 512 | 128 |
| Encoded wire bytes | 1 MiB | 256 KiB |

The existing 256 KiB/client one-second wire window is an additional constraint. A failed transport
send blocks only that client's FIFO for the rest of the delivery cycle; healthy clients and the
command gateway continue. A server tick drains old backlog before commands, newly committed output
after commands, and post-command reliable replication before transient snapshots when capacity
remains. This preserves FIFO order without adding an unconditional tick of latency. Tick reports
retain initial/final messages and bytes, attempted/delivered bytes, retry/failure counts, wire-window
and tick-limit deferrals, blocked clients, and overload disconnect identities. Focused tests prove a
two-client four-message burst gives each client one message per tick and reaches zero backlog on the
second tick.

This is bounded application scheduling, not receiver flow control or congestion control. The memory
limits follow the resource-bounding rationale in
[RFC 9000 section 4](https://www.rfc-editor.org/rfc/rfc9000#section-4); rotating per-client service
also avoids the single-stalled-association failure described for shared socket buffers in
[RFC 6458 section 3.1.2](https://www.rfc-editor.org/rfc/rfc6458#section-3.1.2). Transport pacing,
loss recovery, and bytes in flight remain separate responsibilities.

Local single-player startup is the one explicit exception to steady-state drain pacing. Before an
in-memory client is published to the runtime, its already-capped bootstrap FIFO is drained
synchronously. With the default envelope this returns the collision interest and player seed
together. If a deliberately smaller cap can admit collision chunks but not the atomic player-state
batch, session creation retains a connected client with that state marked pending and retries it on
ordinary ticks; it never publishes a prediction seed ahead of missing collision data. This path is
rejected for socket-backed clients, does not admit data beyond the normal global/per-client backlog
caps, and has its own profiling zone. Remote bootstrap is incremental and subject to ordinary tick
and one-second limits.

### Chunk subscriptions and snapshot publication

Chunk residency now has a deterministic per-client planning contract below the runtime adoption
layer. The default desired cylinder has horizontal radius 2 and vertical radius 1 (39 chunks); a
wider 3-by-2 retain cylinder supplies hysteresis. Policy validation imposes a 4,096-chunk hard
ceiling, while the default client cap is 128 chunks and each planning update may add 4 and remove
16. Existing subscriptions must be unique and already within the cap.

The pure planner adds nearest desired chunks first and removes chunks outside the retain volume
farthest first. It reports quota-deferred additions/removals and capacity-deferred additions
explicitly. Hysteresis cannot permanently starve current interest: if retained, non-desired chunks
fill the cap, the planner spends remaining removal budget evicting the farthest of them so desired
chunks can enter. Output subscriptions are sorted, unique, and bounded even at signed 64-bit world
coordinate limits. Tracy builds expose the planning zone, subscription count, and deferred-addition
count.

`chunk.subscription_remove.bin.v1` is the reliable, versioned unsubscribe payload. Client intake
selects binary snapshots, legacy snapshots, and removals in original queue order rather than
draining each type separately. Applying a removal discards any partial snapshot assembly, the
remembered remote revision, and the resident client chunk; synchronization and inspection report
the count separately from snapshot slices. This order is required when an unsubscribe follows
already-queued slices in the reliable FIFO.

`ServerRuntime` now owns one persistent sorted subscription set and publication table per player
connection. The exact player chunk is the planning center. Ordinary ticks apply the configured
addition/removal quotas once per client, rotate client service and snapshot candidates for fairness,
and publish only loaded subscribed chunks. A publication is keyed by chunk load identity and content
revision, so comparing authoritative state to the per-client table retries deferred work and
self-heals without relying on a lossy one-tick changed list. When a subscribed authoritative chunk
disappears, or interest leaves the retain volume, its reliable removal must enter the FIFO before
the server forgets the client publication.

A per-tick cache encodes each coordinate/identity/revision snapshot once and reuses the immutable
slice payloads for every interested recipient. Each client's 32-slice publication enters the
reliable backlog atomically. Exact admission pressure defers the candidate without creating client
assembly debris; nearest additions, farthest removals, stale/current publications, payload bytes,
codec operations/time/overshoot, time-budget deferrals, and reliable admission deferrals remain
inspectable. Ordinary ticks stop new cache-miss serialization at a configurable 4,000 us global
boundary; already encoded cache hits may still publish, and rotating service prevents persistent
client starvation. Direct local startup plans the complete bounded desired set and retains its
synchronous codec exception, while transport-driven joins use the ordinary transition and time
quotas.
Because missing client chunks are solid for movement prediction, loaded spawn-interest snapshots
are queued before the atomic assignment/movement/inventory seed. Transient snapshots exclude a
client until that seed has been published. A remote transport welcome can precede the seed, so
gameplay readiness requires both protocol connection and a local authoritative player snapshot.

Focused runtime coverage proves bounded unrelated-far-chunk exclusion, quota-limited teleport
convergence, ordered client eviction, one encoding shared by two recipients, atomic deferral and
recovery behind constrained reliable and serialization budgets, and collision-first recovery with
a 64-message bootstrap cap. The deterministic eight-client macrobenchmark adds clustered sharing,
disjoint relevance, rapid traversal, exact per-client wire bytes, clean-host P99, and bounded
backlog evidence.

Committed voxel operations now carry owner-only chunk routing metadata. Before command execution,
`ServerRuntime` publishes sorted chunk-interest rules from each client's exact-current publication
table. The same rule filters both the immediate reliable event batch and its typed world delta;
unpublished or far clients receive neither. Spatial event count, relevant and filtered
event-recipient pairs, and filtered logical payload bytes are retained in relevance inspection, and
Tracy builds plot spatial events plus delivered and excluded pairs. A two-client runtime contract
teleports one client out of interest, proves the near/far 1:1 decision and absence of both payload
types at the far client, then returns it and verifies exact authoritative recovery through a full
chunk snapshot.

The separate committed voxel event/delta relevance path is therefore closed. Remaining M6 work
includes impaired-network and hot-edit P99/SLO evidence plus long-soak coverage. See
[Multiplayer chunk-subscription benchmarks](../performance/multiplayer_chunk_subscription_benchmarks.md).

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
loop. Reliable application queues have strict global/per-client message and byte caps plus fair
per-tick drain limits; their defaults remain safety rails pending scale calibration.

Statistics expose encoded bytes/messages, initial/final reliable backlog bytes and messages,
tick/window deferrals, blocked and overload-disconnected clients, drops, retransmits,
malformed/rate-limited traffic, reassembly ownership, command/replication counts, prediction
corrections, and related maintenance activity. Exact measurements from a single acceptance run
belong in test artifacts or benchmark records, not this architecture contract.

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
