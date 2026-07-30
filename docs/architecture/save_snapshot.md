# Save Snapshot Sections

The save snapshot model keeps major world representations in separate typed sections.

Implemented foundation:

- metadata
- voxel palette manifest
- chunk edit records
- build piece records
- entity save records
- inventory records
- cargo records
- workpiece records
- assembly records
- process records
- fire records
- mod state records
- missing-prototype placeholder records

The validator checks:

- save metadata validity
- voxel palette type/prototype uniqueness and reserved-air rules
- stable save ids
- duplicate permanent ids across saved world objects
- duplicate chunk edit coordinates
- chunk edit delta decoding and coordinate matching
- build piece record invariants, including valid socket/port/tag ids and construction state
- entity save record invariants, including stable ids, valid prototypes, known entity kinds, and
  finite non-zero-scale transforms
- duplicate inventory owners
- inventory stack prototype references and count bounds
- cargo finite position, mass, volume, stability, known transport mode bitmask, and hazard tag
  invariants
- duplicate workpiece ids
- duplicate process ids
- duplicate fire owner/component ids within the fire section and fire state/time invariants; a
  fire id is not a second globally unique world object id
- duplicate mod state keys and state owned by neither an enabled mod nor the built-in engine
- missing-prototype placeholder shape and per-kind/stable-id uniqueness
- prototype references through `PrototypeRegistry`
- expected prototype kinds for each section
- non-empty opaque payloads where needed
- workpiece grid payload decoding and shape matching
- inventory and process owners
- process instance state, work/time, and interruption invariants
- process slot prototype and count validity
- assembly roots and parts against saved build pieces
- assembly process slots against saved processes and their owning assembly
- assembly part and port record uniqueness

This is the engine-side contract that binary or database-backed save files should
preserve: typed sections, stable ids, prototype references, and rebuildable derived data.

`SaveTextCodec` can encode and decode the current `SaveSnapshot` as deterministic text.
This format is retained for tests, golden files, compatibility fixtures, and inspector tools.

`WorldSnapshotBridge` exports authoritative `WorldState` data into these typed sections
and imports snapshots back into world state. Runtime-only identities such as entity
runtime handles and session net ids are regenerated on import. Its raw chunk restoration path
applies edit deltas without marking chunks save-dirty or replication-dirty merely because state was
imported. That path applies to an already resident baseline; if no chunk exists, it creates an empty
one. Normal streamed world loading must generate the deterministic baseline first and use
`insert_generated_with_saved_edits` so the first saved `previous` cell is checked against it.

Opening a saved game also materializes a session-owned `VoxelPalette` from the persisted palette
manifest before the server or client world is created. Existing numeric voxel types therefore keep
their saved prototype assignments, prototypes introduced by the current content set are appended,
and prototypes that no longer exist resolve to named missing-voxel definitions. Rendering,
collision, selection, command validation, and subsequent saves all consume that same restored
palette for the lifetime of the session.

`SaveBinaryCodec` encodes and decodes the current `SaveSnapshot` as a versioned binary
payload with explicit typed sections. `FileSaveDatabase` uses this representation for each
committed snapshot generation; future storage backends must preserve the same boundaries.

Text and binary codecs preserve persisted cargo transport mode bitmasks exactly. Unknown
transport bits are rejected by snapshot validation instead of being silently dropped.

`FileSaveDatabase` stores full binary snapshots and independent chunk-delta payloads in a
directory-backed, generation-staged layout. The external chunk table is authoritative whenever its
index exists, including when that index is intentionally empty.

Save snapshots can be inspected through the debug inspection layer to report section
counts and validation issues.

`heartstead_save_inspector` loads snapshot text files, validates prototype references
against the current mod registry, compares saved mod metadata with active prototype
fingerprints, and prints the same structured inspection data used by debug tooling. Its
`--slots <save_slots_root>` mode lists file-backed save slots and renders both aggregate catalog
inspection and per-slot metadata, layout, generation, and chunk-delta inspection fields.

`heartstead_world_inspector` loads the same text snapshot format, imports it through
`WorldSnapshotBridge`, and prints live `WorldState` inspection data. It validates active mods and
saved prototype references before materializing runtime records. Use it when debugging runtime
database counts, dirty regions, and allocator identity after snapshot import.
