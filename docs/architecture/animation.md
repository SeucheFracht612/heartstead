# Animation

Heartstead cooks glTF 2.0 geometry, materials, skins, and animation clips into the bounded
`heartstead.model.v5` runtime asset described in [assets.md](assets.md). Runtime animation never
parses JSON and never receives source-container paths.

Animation support is independent from skinning. The runtime derives four separate model
capabilities:

- animation clips are present;
- one or more used primitives have a skin;
- morph targets are present;
- animation channels target node translation, rotation, or scale.

These capabilities may occur in any combination. An unskinned model with node-transform channels
is a rigid-node animated model, not a static model. A visual is static only when it has no active
animation mapping.

## Shared node pose

`NodePose` is the common runtime pose for rigid-node and skinned animation. It stores one local TRS
value and one morph-weight vector per model node. Sampling starts from the authored node transforms,
so properties omitted by a clip retain their authored values.

The animation boundary follows the glTF interpolation rules:

- STEP channels hold the preceding key.
- LINEAR translation and scale channels use component interpolation.
- LINEAR rotation channels use shortest-path quaternion spherical interpolation.
- CUBICSPLINE channels use cubic Hermite interpolation with tangents scaled by the keyframe time
  interval. Quaternion results are normalized after component-wise Hermite evaluation.
- Morph-weight channels use the same STEP, LINEAR, and CUBICSPLINE rules.

Pose blending interpolates translation and scale and uses shortest-path quaternion interpolation
for rotation. It happens in local node space before hierarchy evaluation. The evaluator then walks
parents before children:

`node_global = parent_global * node_local`

Root nodes use their local matrix directly. One evaluated model-space matrix array therefore feeds
both consumers:

- rigid primitives use their owning node's global matrix as the render object's model-local
  transform;
- skinned primitives build their joint palette from the same global matrices.

For a low-level mesh-local skinning consumer, the palette convention is:

`inverse(mesh_global) * joint_global * inverse_bind`

The retained whole-entity presentation path uses model-space palettes:

`joint_global * inverse_bind`

This keeps the entity's floating-origin transform on the render object while avoiding decomposition
of animated node matrices back into renderer TRS values.

## Presentation and rendering

`AnimatedModelPresentation` is the game-to-renderer ownership bridge. It uploads each model
primitive once and retains one render object per primitive and observed entity. A skinned primitive
also owns a palette; an unskinned primitive remains on the ordinary static-mesh shader path and
receives its animated node matrix through `RenderObjectProxy::model_transform`.

The renderer composes:

`entity_transform * visual_scale * model_transform`

The same result drives culling and the opaque, alpha-tested, and transparent scene layers. Morph
deltas are applied before skinning on the GPU. Unskinned animated primitives use conservative
per-primitive local bounds containing base vertices and the absolute extent of all morph targets,
expanded by the visual's `bounds_padding`, then transformed by the current node matrix. Skinned
primitives continue to use the padded whole-model animated bound.

`visual_scale` is the entity visual's positive uniform `model_scale`, which defaults to identity.
It is presentation-only and wraps both rigid node matrices and skinned palettes, so an asset can be
matched to gameplay world-unit proportions without modifying the shared model, animation pose,
controller transform, or collider.

`RenderObjectFlags::cast_shadow` controls extraction into the directional-cascade and local
spotlight shadow passes. Those passes consume the same composed object matrix, morph data, alpha
cutoff, two-sided material state, and skin palette as visible scene layers, keeping animated and
cutout geometry aligned with its shadow.

The retained `RenderScene` owns generation-safe skin palettes. A frame uploads each visible palette
once even when several instances share it, then references the absolute buffered-ring offset from
instance metadata. Palette and instance capacities are explicit configuration budgets; missing,
mismatched, or over-budget skinned instances fail closed and are visible in renderer statistics.

`skin_model_vertex` remains the CPU reference implementation for position and normal linear-blend
skinning. The renderer uses GPU skinning through the unified vertex contract: four 16-bit joint
indices and four normalized 16-bit weights plus a buffered palette range. Static and rigid-node
instances take the zero-palette branch and remain batch-compatible.

## Locomotion, transitions, and root motion

An animated player visual maps the semantic idle, walk, run, jump, fall, and swim roles to named
clips. All six roles are required, but several roles may name the same clip. The locomotion state
machine is agnostic about whether a clip drives rigid nodes, joints, morph weights, or a combination.
`transition_ticks` blends the previous and current sampled local poses before hierarchy evaluation.

Player and generic entity motion snapshots carry bounded locomotion state and timing, not node
poses or bone palettes. Clients resolve clip names, sample, blend, and evaluate the cooked model
locally. The authoritative controller remains responsible for entity position, velocity, collision,
and networking.

Player presentation uses in-place animation. Horizontal translation is removed from animated
hierarchy roots, skin skeleton roots, and common root-motion carrier nodes before matrices are
evaluated; authored vertical motion and child articulation remain intact. This prevents a
locomotion clip from moving the visual away from its controller or applying movement twice.

The base Kenney player exercises the rigid branch: six unskinned mesh primitives are attached to
animated body-part nodes, all 27 named clips survive cooking, and the player mapping selects
`idle`, `walk`, and `sprint` with the normal locomotion roles. Its source bounds are approximately
2.73 world units tall, so the base visual applies `model_scale = "0.66"` to present it at
approximately 1.80 units while leaving gameplay dimensions authoritative. The existing test animal
exercises the skinned branch. Both go through the same sampling, blending, hierarchy, morph, and
presentation code.

Equipment sockets, layered animation, deterministic update budgets, interaction targets, and
stateful machine/container mappings are documented in
[Animation, Equipment, and Stateful Visuals](../animation_equipment_and_visual_states.md).
