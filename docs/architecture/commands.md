# Server-Authoritative Commands

Clients request meaningful world changes with commands. The authoritative server
validates those commands, applies mutations through `WorldOperation`, then emits events
for replication and save dirtiness.

Implemented foundation:

- `CommandEnvelope`
  - sequence id
  - sender net id
  - command type
  - payload string, opaque to transport and replay
  - client timestamp

- `CommandPayload`, `CommandPayloadTextCodec`, and `CommandPayloadBinaryCodec`
  - one structured key/value command model shared by tools, replay, and live networking
  - deterministic text encoding for fixtures, replay, and the command-handler boundary
  - bounded versioned binary encoding for live transport
  - lowercase validated field keys
  - percent-escaped delimiters and line breaks
  - duplicate-key and malformed-escape rejection
  - string field values that handlers parse into command-specific typed data

- `CommandExecutionContext`
  - executor role
  - server timestamp
  - save id allocator
  - prototype registry
  - authoritative `WorldState` pointer for handlers that mutate world data

- `ServerCommandDispatcher`
  - registers command descriptors
  - rejects unknown commands
  - rejects mutating commands outside authoritative-server execution
  - runs handlers with a `WorldOperation`
  - commits mutating operations only when an event is emitted and save/replication dirty states are
    marked
  - returns a local operation trace with stages, mutation descriptions, derived updates, and dirty
    flags for diagnostics and replay reports

- `WorldCommandRegistry`
  - registers engine-owned command handlers
  - `world.set_voxel`
    - decodes exactly `chunk`, `voxel`, and `prototype` fields
    - accepts `prototype=air` to clear a cell; every other value must be a stable voxel
      prototype id
    - mutates terrain chunks through `WorldState`
    - marks chunk rebuild dirty regions through `ChunkDatabase`
  - `build.place_piece`
    - decodes a structured command payload with `prototype` and optional transform fields
    - reserves a stable save id
    - validates the build-piece prototype through `PrototypeRegistry`
    - materializes prototype material tags, room contribution tags, and named network ports
    - inserts a `BuildPieceRecord` into the build-object database
    - marks room and port-related network dirty regions for derived-system rebuilds
  - `build.complete_piece`
    - decodes a structured command payload with a stable build-piece object id
    - transitions an existing build piece to `complete`
    - rejects missing or already-complete build pieces
    - marks room and port-related network dirty regions
    - rebuilds derived spatial networks from complete build-piece and assembly ports
  - `workpiece.edit_cell`
    - decodes a structured command payload with `workpiece_id`, `operation`, `coord`, and
      optional `cell` fields
    - finds the stable workpiece record in `WorldState`
    - applies the local grid operation through `WorkpieceGrid`
    - emits a workpiece edit event without touching terrain chunks or reserving build-object ids
  - `workpiece.finish`
    - decodes `workpiece_id` and an optional stable `pattern` prototype id
    - validates session ownership and rejects already-committed workpieces
    - matches the private grid against the prototype-derived pattern library, derives output
      metadata and byproduct quantity, and commits the workpiece once
  - `inventory.transfer_items`
    - decodes a structured command payload with source owner, destination owner, source slot,
      destination slot, and count fields
    - finds owner-keyed inventory records in `WorldState`
    - transfers an exact stack count through the shared inventory transfer helper
    - emits an inventory transfer event without touching cargo or physical transport state
  - `process.start`
    - decodes a structured command payload with owner and process prototype fields
    - validates that the owner `SaveId` references an existing saved world object
    - validates the process prototype through `PrototypeRegistry`
    - reserves a process id through `WorldState`
    - inserts a timestamped `ProcessInstance`
  - `process.advance_all`
    - accepts an empty command payload; client-supplied process rate modifiers are rejected
    - advances all running processes against `WorldState::world_time`
    - materializes process prototype definitions for room, power, and quality requirements
    - resolves per-process room and power modifiers from `WorldState` derived rooms and networks
    - emits a batch process advancement event when at least one process changes state
    - rejects zero-change calls so mutating commands do not commit empty transactions
  - `world.sleep`
    - decodes an `hours` value in the inclusive range 1 through 24
    - advances only authoritative world time using the configured world-time scale
    - does not itself evaluate every lazy process or fire; those systems consume the new time at
      their normal evaluation boundary
  - `cargo.create`
    - decodes a structured command payload with a cargo prototype field and optional `position`
      field
    - validates the cargo prototype through `PrototypeRegistry`
    - materializes a `CargoDefinition`
    - reserves a stable cargo save id and inserts a `CargoRecord`
  - `entity.spawn`
    - decodes a structured command payload with an entity prototype field and optional
      `position`, `rotation`, and `scale` fields
    - validates the entity prototype through `PrototypeRegistry`
    - validates the initial transform before reserving runtime, network, or save identity
    - reserves runtime and session net identity through `WorldState`
    - reserves a stable save id only for persistent entity definitions
    - inserts a validated `EntityRecord`
  - `assembly.create`
    - decodes a structured command payload with assembly prototype, root build piece, and named
      build-piece parts
    - materializes and validates an `AssemblyDefinition`
    - derives assembly parts and required ports from placed build pieces
    - reserves a stable assembly save id
    - marks assembly/network derived data dirty and rebuilds derived spatial networks
  - `assembly.start_blueprint`
    - decodes `prototype` and completed root build-piece `root` ids
    - creates a stable assembly record in blueprint state from its prototype layout
  - `assembly.place_part`
    - decodes `assembly`, local part `name`, and completed `build_piece` ids
    - verifies the part is declared by the prototype and occupies its ghost slot
  - `assembly.advance_stage`
    - decodes an `assembly` id and advances its prototype-defined construction stage
    - materializes the complete ready assembly when the final stage is reached
  - `assembly.transition`
    - decodes `assembly`, target `state`, and optional `reason`
    - applies the assembly state machine, including the required reason for a failed transition

Payload coordinates and vectors use pipe-delimited components (`x|y|z`). `assembly.create`
encodes named parts as a comma-delimited `name:build_piece_id` list. Build, cargo, and entity
commands accept either the legacy `position=x|y|z` form or the anchored
`position_anchor=chunk_x|chunk_y|chunk_z` plus `position_local=x|y|z` form, but never both.
These are command-specific schemas inside the escaped `CommandPayloadTextCodec` envelope; they
are not raw transport delimiters.

The game layer additionally registers `player.input` and the interaction module's
`voxel.place`/`voxel.remove` intents. Those handlers validate game-specific input before routing
terrain mutation through the same authoritative world path; they are not aliases that bypass the
engine dispatcher.

The command dispatcher is not responsible for sockets or packet delivery. Transport
messages are converted into `CommandEnvelope` values, then the dispatcher validates and
executes them on the authoritative server.

The transport host rejects replayed or out-of-order reliable command sequences before
they reach the dispatcher. Command handlers still remain responsible for semantic validation,
authority checks, transactional mutation, and id reservation.

Committed operation events are converted into `ReplicationBatch` payloads by the host
session so connected clients can observe authoritative world changes.

`CommandReplayRunner` executes recorded command envelopes through this dispatcher so
command behavior and transaction traces can be inspected without a live transport connection.

`CommandEnvelope` retains the canonical payload string at the dispatcher and replay boundary.
Live networking uses `CommandPayloadBinaryCodec` on the wire and reconstructs that same
canonical command model before dispatch; tools and replay retain the deterministic text codec.
Transport encoding can therefore evolve without changing handler ownership or world-operation
semantics.
