# Vegetation authoring

Vegetation species are `vegetation_species` prototypes. Runtime geometry always comes from a
production-cooked model; the game never parses glTF while presenting a patch.

```toml
kind = "vegetation_species"
id = "example:vegetation/reed"
display_name = "River Reed"
vegetation_kind = "kelp"

lod.0.model = "example:models/vegetation/reed.gltf"
lod.0.maximum_distance = "48"
lod.0.transition_width = "6"
lod.0.density = "1"

lod.1.model = "example:models/vegetation/reed_billboard.gltf"
lod.1.maximum_distance = "128"
lod.1.transition_width = "16"
lod.1.density = "0.35"
lod.1.impostor = "true"

variation.scale_min = "0.8"
variation.scale_max = "1.3"
variation.yaw_degrees = "360"
variation.mirror = "true"
variation.color_min = "0.7,0.8,0.7,1"
variation.color_max = "1,1,1,1"

wind.stiffness = "0.1"
foliage.transmission = "0.5"
density.fade_start = "64"
density.fade_end = "128"
shadow.lod = "0"
receives_weather = "true"

growth.states = "young,mature"
growth.young.scale = "0.45"
growth.mature.scale = "1"
```

Supported kinds are `grass`, `flower`, `crop`, `bush`, `forage`, `tree`, `fallen_tree`, `vine`,
`kelp`, and `magical_plant`. Kinds are semantic: placement and gameplay systems can select them
without inspecting model names.

LOD maximum distances must increase. `transition_width` is a dithered overlap; `density` is a
stable per-instance acceptance fraction. Use lower density in distant LODs so a field becomes
sparser without a synchronized pop. `density.fade_start` and `density.fade_end` assign a stable
per-instance cutoff across the interval, avoiding a hard field edge. `shadow.lod` is the furthest
LOD permitted to cast shadows.
Impostor LODs use the same retained-instancing path and are marked for camera-facing presentation.

Patch variation hashes the patch seed and logical instance index. Rebuilding an unrelated chunk,
moving the floating origin, or unloading and reloading the patch cannot change placement, scale,
yaw, mirroring, color, wind phase, or LOD membership. A terrain placement system may provide a
height sampler when constructing a patch; height affects Y only and does not reseed the plant.

The renderer batches matching mesh, material, layer, sidedness, and shadow policy. Authors should
reuse LOD models and material slots across a species where possible. Alpha-tested foliage should
be two-sided and should preserve alpha coverage in its cooked mip chain.

`receives_weather` enables the shared lit-material rain wetness and snow accumulation response.
Disable it only for sheltered or supernatural species. The renderer currently uses a single-sample
FXAA path, so alpha-to-coverage is not exposed as an inactive authoring switch.
