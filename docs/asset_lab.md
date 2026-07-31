# Asset Lab

Asset Lab is the production presentation inspector. It loads the same content report, production
cooked store, renderer, PBR shaders, terrain material assets, model presentation, visual-prefab
state mapping, and particle presentation used by the game.

Build and launch:

```bash
cmake --build --preset default-debug --target heartstead_asset_lab

./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --prefab base:visuals/player \
  --preview character \
  --lighting overcast \
  --debug roughness
```

Use `--list` for every accepted name. Preview modes cover static and animated models, characters,
equipment/socket validation, terrain materials, vegetation, particles, materials, textures, LODs,
and visual-prefab state. Repeat `--state CHANNEL=VALUE` to reproduce gameplay-owned state and use
`--lod N` to isolate an external prefab LOD.

Native terrain-material preview builds editable voxel geometry through the production chunk mesher
and displays a tile for every supported surface-state bit. Native texture preview decodes the active
catalog source only for its inspection quad while the headless inspection validates the versioned
cooked texture, mip chain, compression metadata, and memory estimate. Gameplay loading remains
cooked-only.

Visual-prefab preview settings supply the initial lighting preset, camera distance, and gameplay
state values unless the command line overrides them. Equipment preview validates the named socket
and applies its full bind transform.

Lighting presets are studio, overcast, noon, sunset, night, fire-lit interior, cave, forest shade,
rain/wetness, snow/frost, and underwater. Debug views include the PBR channels, UV sets, tangents,
vertex colors, mip level, texel density, texture residency, LOD, bounds, skeletons, skin weights,
shadow cascades, local-light tiles, and overdraw visualization.

Headless inspection validates imports without a display and prints model/texture format, source and
cooked paths, runtime memory estimate, geometry/material/animation/socket/LOD counts, and selected
prefab metadata:

```bash
./build/default-debug/apps/asset_lab/heartstead_asset_lab \
  --headless --asset base:textures/voxels/grass.png --preview texture
```

`smoke.asset_lab` exercises this route in CTest. Native mode uses Vulkan validation when available.
