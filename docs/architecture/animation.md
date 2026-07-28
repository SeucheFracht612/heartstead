# Animation

Heartstead cooks glTF 2.0 geometry, skins, and animation clips into the bounded
`heartstead.model.v1` runtime asset described in [assets.md](assets.md). Runtime animation never
parses JSON and never receives source-container paths.

The animation boundary follows the glTF interpolation rules:

- STEP channels hold the preceding key.
- LINEAR translation and scale channels use component interpolation.
- LINEAR rotation channels use shortest-path quaternion spherical interpolation.
- CUBICSPLINE channels use cubic Hermite interpolation with tangents scaled by the keyframe time
  interval. Quaternion results are normalized after component-wise Hermite evaluation.
- Missing channels retain the model bind pose.

`SkeletalPose` stores one local TRS value per model node. Pose blending interpolates translation
and scale and uses shortest-path quaternion interpolation for rotation. Hierarchy evaluation then
produces global node matrices. A skin palette is produced in the primitive mesh node's local space:

`inverse(mesh_global) * joint_global * inverse_bind`

This keeps one entity/object transform outside the palette and lets multiple primitives rooted at
different model nodes share the same animated hierarchy. Palettes—not replicated bone transforms—
are presentation data. Replication carries a bounded locomotion state and phase; clients sample and
blend their local copy of the cooked clips.

`skin_model_vertex` is the CPU reference implementation for position and normal linear-blend
skinning. The renderer's GPU path must remain within tolerance of this reference on golden inputs.
GPU skinning is the default direction because the renderer already batches static instances and
uploads storage-buffer data each frame; CPU skinning remains a correctness oracle and a fallback
for tests/headless tools rather than a per-frame vertex rewrite.
