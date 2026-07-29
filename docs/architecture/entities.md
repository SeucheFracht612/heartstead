# Entity Architecture

Entities are runtime-oriented dynamic objects:

- players
- animals
- creatures
- carts
- boats
- dropped items
- projectiles
- temporary physics objects

Implemented foundation:

- `EntityDefinition`
  - materialized from entity prototypes
  - stable prototype id
  - entity kind
  - persistent/transient identity policy
  - creates validated `EntityRecord` values with the right save-id rules

- `EntityRecord`
  - runtime handle
  - network session id
  - optional save id for persistent entities
  - prototype id
  - entity kind
  - world transform for position, rotation, and scale
  - persistence and sleeping state

- `RuntimeHandleAllocator`
  - allocates runtime-only handles separate from saved identity

- `EntityNetIdAllocator`
  - allocates session-only entity network ids separate from saved identity
  - owned by `WorldState` so authoritative spawns do not reuse entity net ids

- `entity.spawn`
  - server-authoritative command that validates an entity prototype and optional transform fields
  - reserves a runtime handle and entity net id
  - reserves a save id only for persistent entity definitions
  - inserts a validated `EntityRecord`

- entity save records
  - persist stable save id, prototype id, kind, sleeping state, opaque entity state, and transform
  - keep runtime handles and session net ids out of permanent save data

- `entity_visual` presentation definitions
  - bind entity prototypes to production-cooked static or skinned models
  - map locomotion roles to unique authored clip names
  - are synchronized for replicated scene objects by `ModelPresentationSystem`
  - use the cooked `base:visuals/fallback` marker and renderer error material for an unresolved
    entity prototype, logging each unresolved ID once

- `PhysicalResourceRecord`
  - represents compound dynamic resources such as felled trees without turning every segment into
    a separate physics body
  - keeps stable resource save id separate from runtime `PhysicsBodyId`
  - creates one compound physics body for falling/settling behavior
  - converts settled or frozen resources into ordinary cargo records through the shared cargo
    language

The entity system should not be used for every terrain voxel, inventory slot, crop cell,
or workpiece cell. Persistent entity definitions create records that require save ids;
transient entity definitions create records that must not claim save ids.
