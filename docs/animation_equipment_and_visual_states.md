# Animation, Equipment, and Stateful Visuals

This document is authoritative for the Renderer V2 character and stateful-presentation path.
Gameplay owns state. The renderer samples, composes, caches, attaches, and presents that state; it
does not decide what an entity has equipped or whether a machine is active.

## Runtime data flow

1. Gameplay stores equipment in `items::EquipmentLoadout` and machine/container channels in
   `entities::VisualStateComponent`.
2. Player loadouts are retained by `ClientRuntime`. Non-player visual state is replicated in
   `entity.motion_snapshot.v2`; the decoder remains compatible with v1 packets.
3. `ClientPresentation` maps gameplay state into `RenderObjectSnapshot`. Revision preservation
   compares transforms, locomotion, layers, equipment, state channels, and animation importance.
4. `ModelPresentationSystem` resolves cooked visual-prefab data, publishes character poses, and
   creates retained base, equipment, and stateful model presentations.
5. `AnimatedModelPresentation` samples locomotion/state clips, cross-fades state changes, composes
   masked override/additive layers, emits events, builds skin palettes, and reuses cached poses when
   the deterministic animation budget defers an update.

## Animation layers and events

`AnimationLayerSnapshot` contains a clip role, optional mask name, normalized 16-bit phase,
normalized 16-bit weight, looping flag, and override/additive mode. Layer order is stable and is
applied after the locomotion or selected state clip. An additive layer is evaluated relative to the
bind pose unless a reference playback is supplied through the lower-level animation API.

Prefab masks use descendant roots:

```toml
animation_masks.upper_body = "Spine,ShoulderHarness"
```

Prefab events use normalized phases and are crossed correctly across looping wraparound:

```toml
animation_events.walk.left_foot = 0.25
animation_events.walk.right_foot = 0.75
```

State animation mappings are independent from model, group, and material decisions. They blend
from the retained pose for `transition_ticks`:

```toml
states.activity.active.animation = "Work"
states.activity.active.priority = 30
```

## Deterministic animation budget

`AnimationBudgetSettings` defines a maximum pose-evaluation count and full, half, quarter, and
distant update bands. Candidates are ordered by forced-update status, gameplay importance,
distance, and stable object ID. Distant evaluations are phase-staggered by object ID. The scheduler
never uses elapsed wall time, so client behavior remains reproducible. New and teleported entities
always force an evaluation. Deferred entities retain their last pose while their entity transform
continues to update.

The dev game passes its floating-origin `RenderCamera` into model synchronization. Distance is
computed camera-relatively, preserving precision at large world coordinates.

## Equipment authoring

Equipment variants live on the character visual prefab:

```toml
socket_aliases.main_hand = "main_hand"
socket_aliases.off_hand = "off_hand"
socket_aliases.back = "back"

equipment.main_hand.hammer.model = "base:models/production/workshop_hammer.gltf"
equipment.main_hand.hammer.socket = "main_hand"
equipment.main_hand.hammer.stowed_socket = "back"
equipment.main_hand.hammer.secondary_socket = "off_hand"
equipment.main_hand.hammer.two_handed = true
equipment.main_hand.hammer.materials.ForgedIron = "base:materials/ore"
```

Socket nodes in glTF use the `socket_`, `socket.`, or `socket:` naming convention. Rigid equipment
uses the evaluated socket matrix. Stowed equipment switches to `stowed_socket`. Two-handed data
exposes the secondary target for the interaction/IK layer. Skinned equipment maps same-named model
nodes to the character pose and builds an equipment-local skin palette. `hide_groups` suppresses
body-region primitives through per-instance visibility rather than duplicate full character models.
`materials.<model-slot>` creates a cached equipment model variant through the same validated,
parameter-only material override path used by the owning visual prefab.

All equipment models are included in the presentation cooker dependency closure, alongside base,
LOD, state, and impostor models.

## Stateful machines and containers

Every state output resolves independently across all matching channels. Priority only resolves a
conflict for the same output; a high-priority damage model does not suppress an unrelated activity
animation, heat group, or material slot.

The base content includes:

* `base:visuals/workshop_machine`: activity, process, heat, access, power, and damage channels with
  rigid-node animation and visible input/output/heat/damage parts.
* `base:visuals/stateful_crate`: empty/quarter/half/full cargo, open/closed animation, carried straps,
  and mounted brackets.
* `base:visuals/player`: six locomotion clips, a skinned player, upper-body mask, foot events, hand
  and back sockets, and a held/stowed two-handed hammer variant.

Preview these prefabs in Asset Lab with the existing visual-prefab state controls. Direct equipment
preview accepts the cooked equipment model and the resolved socket name.

## Interaction pose contract

`AnimationInteractionRequest` provides large-coordinate-safe effector targets for left/right feet,
left/right hands, primary/secondary tools, hips, and look direction. It identifies grounded, tool,
workstation, mantle, vault, climb, swim, cart, and ship interactions. This is the validated contract
for later IK solvers; no IK solution is fabricated by the current renderer.

## Benchmark

Run the representative scene with:

```bash
./build/default-release/apps/render_benchmark/heartstead_render_benchmark \
  --scene character-workshop --headless --warmup 60 --frames 300 \
  --output build/default-release/benchmark-character-workshop.json
```

The scene contains 128 equipped skinned players and 384 independently stateful workshop machines.
It advances animation phases and activity/heat transitions deterministically.

## Motion vectors

Scene raster passes carry a transient `scene_motion` RG16F attachment alongside linear HDR color.
`SceneRenderSystem` retains the exact clip transform, morph weights, and skin palette submitted for
each stable render object and palette ID. The next frame uploads both temporal samples, so rigid,
skeletal, morph, camera, and floating-origin motion contribute to screen-space velocity. New,
teleported, or previously culled objects collapse history to the current sample and cannot create a
false velocity spike.

Procedurally deformed vegetation, water, and particles currently write zero velocity because their
previous procedural sample is not retained. This is deliberate and does not affect character,
equipment, machine, or ordinary rigid-object motion. The current FXAA pass does not consume motion
vectors; the attachment is available to later temporal anti-aliasing and motion effects without an
RHI change.

Design references: [glTF 2.0 animation and skinning](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html),
[Unreal Engine Animation Budget Allocator](https://dev.epicgames.com/documentation/unreal-engine/animation-budget-allocator-in-unreal-engine),
and [Unreal Engine modular characters](https://dev.epicgames.com/documentation/unreal-engine/working-with-modular-characters-in-unreal-engine).

See [Testing](dev/testing.md) for the focused automated and Asset Lab verification commands.
