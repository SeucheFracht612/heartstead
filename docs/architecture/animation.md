# Animation

Heartstead cooks glTF 2.0 geometry, materials, skins, and animation clips into the bounded
`heartstead.model.v4` runtime asset described in [assets.md](assets.md). Runtime animation never
parses JSON and never receives source-container paths.

The animation boundary follows the glTF interpolation rules:

- STEP channels hold the preceding key.
- LINEAR translation and scale channels use component interpolation.
- LINEAR rotation channels use shortest-path quaternion spherical interpolation.
- CUBICSPLINE channels use cubic Hermite interpolation with tangents scaled by the keyframe time
  interval. Quaternion results are normalized after component-wise Hermite evaluation.
- Morph-weight channels use the same STEP, LINEAR, and CUBICSPLINE rules and are evaluated before
  skinning on the GPU.
- Missing channels retain the model bind pose.

`SkeletalPose` stores one local TRS value per model node. Pose blending interpolates translation
and scale and uses shortest-path quaternion interpolation for rotation. Hierarchy evaluation then
produces global node matrices. Two explicit palette conventions are available. The low-level
mesh-local form is:

`inverse(mesh_global) * joint_global * inverse_bind`

The retained whole-entity presentation path uses model-space palettes:

`joint_global * inverse_bind`

The latter preserves the authored model-node transform inside the palette while the entity's
floating-origin transform remains on the render object. This is the standard whole-model glTF
instance composition and avoids decomposing animated node matrices back into renderer TRS values.
Palettes—not replicated bone transforms—are presentation data. Replication carries a bounded
idle/walk/run/jump/fall/swim state, normalized phase, source state, and transition tick. Clients
interpolate that state, resolve the visual's authored clip names, sample the same cooked clips, and
blend locally.

`skin_model_vertex` is the CPU reference implementation for position and normal linear-blend
skinning. The renderer uses GPU skinning through the existing static-instance path: its unified
vertex contract adds four 16-bit joint indices and four normalized 16-bit weights, while each
`GpuObjectInstance` identifies a range in a buffered skin-matrix storage ring. Static instances
take the zero-palette branch, so static and animated objects remain batch-compatible.

The cooker accepts both glTF influence sets and retains the strongest four normalized influences.
Morph deltas remain in a device-local storage arena and visible instances upload only their current
bounded weight vectors. The vertex shader applies weighted position, normal, and tangent deltas
before linear-blend skinning.

The retained `RenderScene` owns generation-safe skin palettes. A frame uploads each visible palette
once even when several instances share it, then references the absolute buffered-ring offset from
instance metadata. Palette and instance capacities are explicit configuration budgets; missing,
mismatched, or over-budget skinned instances fail closed and are visible in renderer statistics.
The CPU implementation remains the golden correctness oracle for tests/headless tools rather than
a per-frame vertex rewrite.

`AnimatedModelPresentation` is the game-to-renderer ownership bridge. It uploads each model
primitive once, retains one render object and (for skinned primitives) one palette per observed
entity, skips unchanged authoritative revisions, and removes objects before their referenced
palettes. Creation rolls back partial renderer ownership on failure. A headless renderer integration
test covers insertion, locomotion-driven palette replacement, rendering, removal, and shutdown.

Player locomotion travels in the authoritative controller snapshot. Non-player animated entities
use the bounded `entity.motion_snapshot.v1` boundary: network identity, prototype, previous/current
world transform, locomotion state, and simulation tick. Reliable tombstones remove retained client
state. `ClientPresentationSynchronizer` maps both snapshot families into the same presentation
world, so the renderer does not know whether an animated object is a player or an animal. M7 will
replace the current bounded text payloads with the planned binary replication codec without
changing this semantic boundary.

The base test animal is a normal gameplay module and entity prototype. Its server-side wander
system uses a seeded, fixed-step state machine, mirrors transforms into the generic replicated
components, and alternates walking and idle segments so the same transition blending is exercised
outside player control. Two independent headless sessions produce byte-for-byte equal transform
and locomotion trajectories. `entity_visual` definitions bind entity prototypes to model IDs and
named clips. The build production-cooks every declared visual model and dependency, and
`ModelPresentationSystem` renders both the third-person player and test animal through shared
static/skinned presentation without per-entity application wiring.
