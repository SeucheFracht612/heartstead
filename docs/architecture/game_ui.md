# Game UI

Heartstead's game UI is a retained, renderer-neutral widget layer above `UiRenderer`. The widget
tree owns interaction state; `UiRenderer` owns GPU buffers and turns the resulting quads, text, and
scissor groups into draw submissions. Gameplay code never owns renderer resources.

## Widget model

Every widget has a stable `WidgetId`. Rebuilding a screen preserves focus, pointer capture,
text-editing, scrolling, and drag state by ID. The tree supports deterministic row, column, grid,
overlay, and absolute layout with explicit constraints, DPI scaling, inherited clipping, and a
bounded traversal depth.

The initial controls are panels, labels, images, buttons, toggles, sliders, text inputs, scroll
areas, tooltips, and grid slots. Slots can be drag sources and drop targets. Input processing
produces a consumption report so the application knows which pointer, keyboard, text, and
navigation actions must not reach gameplay.

Focus order and directional navigation are part of the widget contract rather than inferred from a
particular device. Pointer and keyboard adapters are live. The same up/down/left/right/accept/cancel
actions are ready for a native gamepad adapter without changing inventory behavior or focus rules.

## Input ownership

`dev_game` routes the platform frame to `GameUiLayer` before evaluating gameplay actions. `Tab`
opens the inventory and releases the pointer; closing it restores captured camera input. While the
modal is open, the client still sends neutral movement input so the authoritative controller does
not retain a previous direction, but movement, jump, interaction, and physical-drop actions are
suppressed.

Pointer capture belongs to the pressed widget until release. Keyboard focus is independent.
Modal focus is scoped to the active screen, and directional movement follows the stable layout
order. The UI layer does not read native X11 types.

## Data-driven skin and batching

Base-mod `ui_panel` prototypes define atlas regions, colors, and nine-slice margins for the carved
panel, button, and slot treatments. `ContentValidationReport` materializes these records into a
`UiSkin`; temporary or minimal mods without a skin receive the built-in storybook fallback.

Nine-slice is expanded into ordinary renderer-neutral UI quads. `UiRenderer` globally rebases
indices and coalesces adjacent geometry with the same atlas and scissor, so an unclipped inventory
grid remains one draw instead of one draw per slot.

## Inventory authority and reconciliation

The server's private `world.initial_snapshot` gives a joining client its player entity and
owner inventory. Movement snapshot v3 also carries the persistent player save ID, keeping runtime
entity identity separate from inventory ownership.

`InventoryUiViewModel` mirrors the latest replicated inventory and submits the existing
server-authoritative `inventory.transfer_items` command. Dragging applies an optimistic local
transfer and records the command sequence. A successful command is replaced by replicated
authoritative state; rejection restores the pre-command state. Left-drag moves the full stack and
right-drag requests a half-stack split. The UI never edits the authoritative `WorldState`
directly.

The HUD reads replicated health and stamina plus mass computed from the replicated item definitions
and stacks. Health is part of movement snapshot v3; the authoritative movement controller remains
the owner of all vital values.

## Observability and budget

`RendererStats` records UI layout time, paint time, widget count, vertices, indices, and draw calls.
`heartstead_ui_benchmark` builds a 2,000-widget inventory-shaped tree for 600 measured frames after
warmup and enforces a `1.0 ms` p95 layout+paint+renderer-build target.

The Release reference run on the current development host measured:

- 0.186 ms median
- 0.206 ms p95
- 0.228 ms maximum
- one draw call

Run it with:

```bash
cmake --build --preset default-release --target heartstead_ui_benchmark
./build/default-release/apps/ui_benchmark/heartstead_ui_benchmark \
  --output build/default-release/ui-benchmark.json
```

## Verification

- `heartstead_widget_tree_tests` covers stable state, constraints, layout, clipping, hit testing,
  capture, focus/navigation, text editing, scrolling, tooltips, drag/drop, nine-slice generation,
  and atlas/scissor batching.
- `heartstead_game_ui_tests` covers accepted transfers, rejection rollback, split-stack behavior,
  UI-before-gameplay input consumption, and HUD bindings to replicated state.
- `smoke.ui_benchmark` keeps the benchmark executable and target assertion in the regular test
  matrix.

## Production UI and maps

The production path uses Noto Sans SDF text as described in [UI rendering](ui_rendering.md). The HUD
paints a discovery-backed minimap; `M` opens a full surface, underground, aerial, or mod-defined map
layer with gameplay-owned markers. UI is composited after tone mapping at output resolution.
