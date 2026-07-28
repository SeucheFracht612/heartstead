# Networking Architecture

Networking is an engine-owned boundary below server-authoritative commands.

Implemented foundation:

- `TransportMessage`
  - message kind
  - reliability channel
  - sequence id
  - payload type
  - bounded opaque payload bytes; live hot paths may use versioned binary codecs while saves,
    tools, and compatibility fixtures may retain deterministic text codecs
  - timestamp

- `TransportEnvelope`
  - sender `NetId`
  - recipient `NetId`
  - transport message

- `ITransportHost`
  - backend-agnostic host transport contract
  - exposes server id, connected clients, send/drain operations, and backend metadata
  - exposes deterministic maintenance polling for backend-owned retransmission/drop work
  - lets host sessions depend on the transport interface instead of the in-memory implementation

- `TransportHostDesc`
  - selects `in_memory` or `external_library`
  - carries backend-specific host configuration
  - is created through `create_transport_host`

- `TransportEndpoint`
  - names the numeric IPv4 bind address and port for the socket-backed foundation host
  - is validated before an external backend can be selected
  - allows port `0` for local tools/tests that need the OS to assign a free bind port
  - keeps network endpoints out of gameplay object identity and save identity

- `TransportCapabilities`
  - reports reliable-channel support
  - reports unreliable-channel support
  - reports whether reliable client command sequence order is enforced before dispatch
  - reports max payload bytes
  - reports max connected clients
  - lets host-session and tooling inspect a selected backend without downcasting it

- `TransportPacketCodec`
  - deterministic packet format for transport envelopes
  - carries sender, recipient, kind, channel, sequence, payload type, timestamp, and raw payload
  - validates packet magic, enum names, ids, payload type, declared payload size, and max payload
    size
  - gives future external backends a stable byte contract without owning command semantics

- `TransportPacketFragmentCodec`
  - deterministic fragment format for already-encoded transport packets
  - splits oversized packet bytes into indexed fragments with packet id, fragment count, total
    byte size, declared fragment payload size, and raw fragment payload
  - validates fragment magic, id, index/count ranges, total packet size, declared payload size,
    max fragment payload size, max packet size, and impossible fragment metadata
  - keeps fragmentation below command semantics so commands, replication, and control messages
    remain opaque to transport

- `TransportPacketReassembler`
  - accepts decoded fragments in any order
  - treats exact duplicate fragments as idempotent
  - rejects conflicting duplicate payloads and mismatched packet metadata
  - emits the original encoded transport packet only after every fragment is present and the
    reassembled byte count matches the declared total

- `TransportReliabilityTracker`
  - tracks reliable-channel transport envelopes that are awaiting acknowledgement
  - permits sequence `0` only for reliable transport control messages such as welcome/disconnect;
    command, command-result, and replication reliability keys still require nonzero sequences
  - emits deterministic `control.transport_ack` payloads as unreliable transport control messages
  - accepts acknowledgements and removes matching pending reliable messages without exposing command
    semantics to the transport layer
  - schedules retransmission attempts from timestamps and retry-delay configuration
  - drops pending messages after a bounded max-attempt count so callers can disconnect, degrade, or
    surface diagnostics
  - remembers recently accepted reliable message keys to detect duplicates and still sends an ACK
    for duplicate deliveries
  - keeps receive tracking bounded so long-running sessions do not retain unbounded packet history

- `TransportControlTextCodec`
  - defines deterministic text payloads for transport-level control messages
  - currently supports `control.server_welcome` and `control.server_disconnect`
  - carries protocol version, server `NetId`, assigned client `NetId`, max payload bytes,
    max clients, unreliable-channel support, and reliable command-order enforcement
  - carries server disconnect reason codes and escaped reason text for graceful session close
  - validates required fields, numeric ranges, boolean flags, protocol version, and ids
  - gives external backends a stable session handshake payload without mixing it into
    gameplay command semantics

- `TransportHandshakeCodec`
  - defines a bounded binary remote hello/challenge/response/accept/reject exchange
  - negotiates protocol/content fingerprints before gameplay traffic is admitted
  - validates an endpoint-bound, expiring challenge cookie before allocating a client session
  - returns a cryptographically random session token that every admitted datagram must carry
  - limits response amplification and handshake response rate before an endpoint is trusted

- `TransportClientSession`
  - is created by accepting a reliable `control.server_welcome` transport envelope
  - validates the expected client `NetId`, envelope recipient, payload type, reliable channel,
    decoded protocol fields, and envelope sender/server id match
  - records the negotiated server id, client id, payload limits, client limit, unreliable-channel
    support, reliable command-order support, and handshake timestamp
  - gives future client connection code a reusable state object before gameplay commands are sent

- `InMemoryTransportHost`
  - implements `ITransportHost`
  - assigns session-scoped client `NetId` values
  - queues client-to-server and server-to-client messages
  - validates connected clients, payload types, max payload size, and max client count
  - records each connected client's last accepted reliable command sequence
  - rejects replayed or out-of-order reliable command messages before the server dispatcher can
    execute them
  - drains deterministic inboxes for tests and local host sessions
  - can apply seeded one-way latency, bounded jitter, and unreliable-message loss without making
    acceptance tests depend on wall-clock scheduling
  - reports encoded bytes, messages, simulated loss, and delayed-delivery backlog in each direction

- Command bridging
  - converts `CommandEnvelope` into a transport command message
  - reconstructs `CommandEnvelope` from a received transport envelope
  - uses the bounded `CommandPayloadBinaryCodec` on the live wire while retaining canonical text
    decode/encode at the existing command-handler boundary
  - keeps authoritative mutation logic in `ServerCommandDispatcher`
  - exposes full dispatch reports for tooling when rejected commands need error details and
    rollback traces
  - leaves command payload bytes opaque to transport

- Command payload foundation
  - `CommandPayloadTextCodec` provides deterministic key/value payloads for structured
    engine commands
  - payload fields validate keys and escape delimiters before handlers parse typed values
  - `CommandPayloadBinaryCodec` provides the versioned, bounded live representation

- Command result foundation
  - `HostSessionCommandResultBinaryCodec` provides the versioned, bounded live representation;
    the deterministic text codec remains available for compatibility fixtures and tools
  - command results report sequence, command type, success/error status, committed-world-mutation
    state, event count, reserved-id count, and escaped error details
  - `host_session_command_result_from_transport` validates command-result message kind,
    reliable channel, sequence agreement, and payload-type agreement before client code consumes
    a response

- `ClientSession`
  - accepts the server welcome and owns the client-side protocol state above transport bytes
  - creates outbound command envelopes with the accepted client `NetId` and monotonically
    increasing local command sequence
  - tracks pending commands until a matching command-result response is received
  - validates server sender id, recipient id, command-result sequence/type, and replication
    envelope shape before queuing decoded client-visible results
  - rejects duplicate or older replication batch sequences before higher-layer world apply code can
    replay authoritative events
  - accepts reliable server-disconnect control messages, clears pending commands, stores the
    disconnect reason, and moves back to disconnected state
  - queues decoded command results and replication batches for future client gameplay/UI layers
  - queues non-event replication payloads as raw envelopes so higher layers can own typed payload
    decoding without adding gameplay/world dependencies to the client protocol session
  - accepts unreliable typed state snapshots while keeping authoritative world-event batches on
    the reliable ordered path
  - can be marked transport-disconnected when client maintenance detects timeout or retry failure
  - exposes an inspectable `ReplicationIntakeReport` for queued batches before they are drained

- `HostSession`
  - owns local host lifecycle state
  - creates its transport through `TransportHostDesc`
  - accepts and disconnects session clients
  - sends a reliable `control.server_welcome` message to each accepted client before gameplay
    command traffic
  - sends a reliable `control.server_disconnect` message before closing a client connection
  - sends client commands through the transport host
  - drains server transport messages
  - polls transport maintenance during each authoritative tick and reports retransmit/drop counts
  - executes commands through `ServerCommandDispatcher` as the authoritative server
  - materializes host command reports from full dispatcher reports, preserving rollback traces for
    failed commands without sending those debug traces to clients
  - sends command-result responses back to clients
  - filters committed world-operation event batches through `ReplicationRelevancePolicy`
  - exposes the current relevance policy and lets the server refresh it before live ticks
  - assigns one server-owned monotonic replication stream sequence independently from each
    client's local command sequence
  - sends per-recipient-filtered events and reserved ids only to relevant connected clients and
    reports filtered clients
  - enforces a fixed one-second encoded-byte ceiling per client, deferring reliable FIFO traffic
    and dropping replaceable unreliable state under backpressure
  - returns an inspectable tick result that cross-checks transport, response, command-report,
    replication, relevance-report, byte, message, drop, backlog, and budget counts

- World command path
  - engine command handlers can receive an authoritative `WorldState`
  - reusable engine commands cover terrain edits and sleep/wake, build-piece placement/completion,
    workpiece edits/finishing, inventory transfers, cargo creation, entity spawns, process starts
    and advancement, and direct or staged assembly lifecycle operations
  - structured payload fields are decoded before command-specific world mutation
  - the in-memory host sample now applies a voxel edit through this path

- Replication foundation
  - `ReplicationBatch` carries committed command sequence, command type, operation events,
    and reserved save ids
  - `ReplicationTextCodec` encodes/decodes deterministic replication payloads
  - `ReplicationRelevancePolicy` gives host sessions a deterministic client interest filter
  - `ReplicationIntake` summarizes decoded client-side replication queues without applying state
  - world replication delta planning classifies event subjects before typed snapshot/state-delta
    payloads are serialized
  - world replication delta materialization copies typed authoritative records into sectioned
    delta snapshots behind bounded, versioned codecs
  - world code converts host tick command reports into per-command typed delta snapshots without
    teaching `HostSession` about build/entity/cargo/inventory/workpiece/assembly/process stores
  - world code can apply typed delta snapshots through separate world stores while preserving
    runtime-only entity identity on the receiving side
  - world code can drain `ClientSession` event intake and apply matching typed deltas without
    teaching client protocol code about world stores
  - typed state-delta transport envelopes use a world-owned
    `replication.world_delta_snapshot.bin.v1` payload type over reliable replication messages,
    decoded by world code rather than net code
  - world code can send complete typed delta snapshots to the same relevant clients reported by a
    host tick, using a host-session replication send hook that only understands transport messages
  - client protocol code can queue typed delta envelopes opaquely, and world code can drain,
    decode, and apply those queued snapshots against matching event batches
  - `WorldReplicationDeltaSnapshotBinaryCodec` is the bounded live representation and embeds the
    existing sectioned save binary snapshot; its deterministic text codec remains for tools and
    compatibility fixtures
  - `BinaryMessageWriter`/`BinaryMessageReader` provide explicit little-endian fixed-width fields
    and bounded varints for live codecs without exposing host ABI/layout
  - commands, command results, world events, authoritative world deltas, player input bundles,
    movement snapshots, and chunk snapshot slices use versioned binary live codecs; input
    redundancy sends the current and recent frames over unreliable transport
  - movement and entity-motion snapshots use unreliable latest-wins delivery; world events and
    store deltas remain reliable
  - server transient snapshot message/byte budgets defer excess work and rotate recipient priority
    so one client cannot permanently starve another
  - world code can derive that filter from simulation subjects and viewer positions without
    teaching transport or host sessions about world representation storage
  - the world layer can refresh a host session with a newly derived filter between ticks as
    viewers move or subjects change LOD
  - host sessions send `replication.world_events.bin.v1` transport messages to relevant connected
    clients, while readers retain the legacy text payload type for compatibility

- External packet backend
  - has an explicit `ExternalTransportHostConfig` with server `NetId`, bind endpoint,
    max payload bytes, max clients, and unreliable-channel policy
  - reports runtime availability through backend info, including sandbox/OS socket denial
  - creates a POSIX UDP packet host when sockets are available
  - keeps the historical public backend name `external_library`, although the current
    implementation is project-owned POSIX socket code rather than a third-party library
  - exposes independent POSIX UDP server and client implementations; the client binds an ephemeral
    local endpoint and connects to a numeric IPv4 server endpoint
  - admits unknown endpoints only through the challenge-cookie handshake, then binds assigned
    client `NetId`, source endpoint, and session token together
  - accepts established datagrams only when claimed identity, source endpoint, and token all match
  - times out incomplete handshakes and idle sessions, sends keepalives, supports graceful
    disconnect, and surfaces malformed/rate-limited datagram counters
  - applies per-client inbound message/byte limits, a global handshake response limit, bounded
    fragment/reassembly ownership, and an amplification limit before cookie validation
  - carries `TransportPacketCodec` payloads, using `TransportPacketFragmentCodec` when a
    backend packet budget is smaller than a command/replication/control packet
  - reassembles incoming fragments before decoding envelopes
  - tracks reliable sends for both server-to-client and local client-to-server directions
  - consumes `control.transport_ack` messages internally before they reach host-session or
    client-session protocol code
  - sends ACK messages for accepted reliable deliveries, suppresses duplicate reliable deliveries,
    and still ACKs duplicates so a sender can stop retrying
  - retransmits pending reliable envelopes from `poll_maintenance(now_ms)` and reports bounded
    max-attempt drops
  - preserves the reliable command sequence guard before authoritative dispatch
  - keeps the public `ITransportHost` contract unchanged so ENet, Steam Networking Sockets, or
    another proven library can still replace or supplement the POSIX backend later

This layer is deliberately separate from command execution. Transport delivers bytes and
session identity; the server command dispatcher validates meaning, mutates world state
through transactions, emits events, and marks save/replication dirtiness.

`NetId` is multiplayer session identity. It must not be written as permanent save
identity, and transport endpoints must not become gameplay object ids.

The POSIX backend now supports directly joinable remote-client-only and dedicated-server
compositions using numeric IPv4 endpoints. Client prediction/reconciliation and delayed remote
interpolation keep presentation responsive independently of the authoritative round trip. The
same controller is used for authoritative and predicted movement, with a bounded input history and
hard-correction diagnostics when replay cannot converge.

The deterministic impaired-runtime acceptance profile is 100 ms RTT with ±20 ms round-trip jitter
and 2% unreliable-message loss. Its fixed 600-tick run accepts 594 of 600 movement inputs, records
one collision-revision correction with a 0.825 m maximum, averages 21.0 KiB/s of encoded
server-to-client traffic, and peaks at 26.6 KiB over one second. The permanent gates are 90% input
acceptance, no more than the documented bootstrap correction, a correction below 1.0 m, an average
below 64 KiB/s, and a hard per-client ceiling of 256 KiB/s.

This remains a controlled-LAN/testing transport rather than a complete public-Internet service.
The current reliability layer is a bounded ACK/resend/drop primitive; it does not provide
congestion control or pacing. The endpoint-bound random session token prevents off-path datagram
injection, but it is not encryption, account authentication, forward secrecy, NAT traversal, or
matchmaking. Public deployment requires those properties (or migration to a proven secure
transport) before accepting untrusted Internet clients.

The challenge validation and pre-validation amplification limits follow the address-validation
principles in [RFC 9000](https://www.rfc-editor.org/rfc/rfc9000); UDP rate/burst guidance follows
[RFC 8085](https://www.rfc-editor.org/rfc/rfc8085). A future congestion/pacing pass should use the
recovery principles in [RFC 9002](https://www.rfc-editor.org/rfc/rfc9002) instead of growing this
foundation into an ad-hoc congestion controller.
