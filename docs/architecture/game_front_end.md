# Game front end, saves, and settings

The Heartstead front end is a state of the primary `HeartsteadApplicationMode`, rendered by the
same retained `WidgetTree` and production UI renderer as session UI. It is not an overlay from the
legacy development application and does not own authoritative world state.

## Main-menu flows

- **Continue** selects `last_world_slot` when it remains valid and compatible, otherwise the most
  recently saved compatible slot. It is disabled with a reason when neither exists.
- **New World** validates a printable display name and unsigned seed, derives a safe unique slot
  ID, and launches the normal generated-world request with persistent ownership.
- **Load World** discovers all safe slot directories. A selected entry shows creation/last-played
  time, save path, game/save versions, migration state, required mods, validation issues, and a
  placeholder thumbnail. Load and Host are disabled for corrupt, schema-incompatible, missing-mod,
  or prototype-hash-incompatible saves.
- **Multiplayer** accepts a validated `host:port`, retains up to sixteen recent addresses, and
  starts the real remote-client transport. Hosted LAN play is available from a selected save.
- **Developer Worlds** searches scenario identity, description, category, and tags; the category
  control cycles through all registered categories. Each item shows its persistence policy and
  launches through `DeveloperWorldRegistry::make_launch_request`.
- **Options** persists existing display, renderer-quality, audio, mouse/controller, UI-scale, and
  accessibility values. Audio and UI accessibility changes apply immediately; window and renderer
  reconstruction settings apply at the next application start.
- **Quit** transitions the application state machine to Shutdown.

Mouse pointing, text input, scroll-wheel overflow, keyboard focus traversal, activation, and back
navigation are all routed through `WidgetTree`. The platform input adapter also maps the input
layer's navigation abstraction, so a controller backend can use the same focus model; the current
native platform implementation does not yet enumerate gamepad devices.

## Application settings

`ApplicationSettingsStore` uses a small versioned bounded text format and atomic replacement. Bad
or out-of-range files do not partly apply: startup reports the error and uses defaults. Settings
belong to application lifetime and live at:

```text
<Heartstead application data>/settings.txt
```

`HEARTSTEAD_DATA_ROOT` overrides the application-data root for tests and developer isolation. The
same root contains the player save catalog. Recent-server and last-world history are settings, not
save-owned state.

## Save catalog safety

`FileSaveSlotCatalog` accepts only safe lowercase namespace IDs and rejects symbolic-link catalog
entries. Rename changes display metadata, not durable slot identity. Duplicate reads the source's
committed snapshot and publishes a new slot; a failed copy removes its incomplete destination.
Delete validates that the exact target is a child of the catalog root before recursive removal and
is only reachable from the confirmation screen.

Discovery is failure-tolerant per slot. Corrupt metadata, database statistics, or committed
snapshots are recorded in `SaveSlotSummary::validation_error`, allowing healthy worlds to remain
usable. Unsafe symbolic links still fail the entire scan because silently traversing or ignoring
them would weaken the catalog boundary.

The current save format does not record a thumbnail or generator version, so the UI says so rather
than inventing metadata. Save-schema migration is not automatic in the player shell; a slot whose
schema differs from the current schema remains visible but disabled.

## Regression coverage

`heartstead_front_end_tests` covers root action availability, valid and invalid menu navigation,
settings round trips/range rejection, snapshot-backed duplication, rename/delete, and corrupt-slot
discovery. `heartstead_menu_headless_smoke` boots the complete executable through the menu without
creating a world.
