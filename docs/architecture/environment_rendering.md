# Environment Rendering

Heartstead environmental presentation is driven by `environment_profile` prototypes. Gameplay
supplies the current biome, weather, time, altitude, underground/aerial/ocean/underwater state, and
overlapping local environment volumes. The renderer evaluates those inputs deterministically into
one frame environment shared by terrain, imported assets, water, vegetation, and effects.

Profiles may author:

- sun, moon, stars, sky colors, ambient light, environment intensity, and rotation;
- distance, height, local, underground, aerial, and underwater fog;
- wind direction, speed, gusts, and turbulence;
- precipitation, clouds, storm intensity, visibility, wetness, and snow;
- water absorption, scattering, refraction, Fresnel, normals, ripples, foam, and underwater grade;
- exposure limits, adaptation, bloom, saturation, contrast, and hazard visual identifiers.

Selectors use `selector.biomes`, `selector.weather`, wrapped `selector.time_start` and
`selector.time_end`, altitude ranges and fades, underground/aerial/ocean/underwater flags, and a
bounded priority. Comma-separated selector values are OR conditions. Different selector classes
combine, and matching profiles are normalized into a continuous blend. Local environment volumes
add a high-priority weighted contribution without changing gameplay state.

The baseline profile must have no restrictive selectors so every valid context has a fallback.
`base:environments/temperate_default` provides that fallback. The starting meadow, rain, cave, and
underwater profiles layer over it.

## Sky, atmosphere, and clouds

The sky is a full-screen analytic pass reconstructed from the inverse view projection. It combines
profile horizon/zenith colors with Rayleigh-like and Mie-like directional scattering, a resolved
sun disc, moon disc, deterministic stars, and two procedurally advected cloud layers. Cloud
coverage, density, storm darkening, sun/moon state, and the shared environment clock come from the
same evaluated profile as world lighting. Height and distance fog then provide aerial perspective
for terrain, meshes, vegetation, water, and effects.

This is deliberately data-driven rather than tied to a fixed day/night shader mode. A weather or
local-volume blend alters the same values consumed by the sky and by the lit world, so a storm
cannot darken the backdrop while leaving the settlement under unrelated ambient light.

## Water

Voxel fluid state remains authoritative for surface height, falling state, and flow direction. The
chunk mesher produces sloped continuous surfaces and flow-oriented UVs. The fluid shader combines
two animated normal samples, rain ripples, angle- and fluid-depth-aware absorption, environment
refraction/reflection, Fresnel, scattering, slope/edge/falling foam, height fog, and profile-driven
underwater grading. All animation uses the renderer environment clock and therefore survives
floating-origin shifts without texture swimming.

`LargeWaterRenderer` supplies lakes, coasts, and oceans that extend beyond ordinary chunk water. A
single retained quadratic grid uses dense near cells and progressively larger far cells, then
submits all visible cells through one instanced draw. Fixed bodies retain an exact
`WorldPosition`; ocean bodies follow a camera-relative, cell-snapped origin. The snap prevents
sub-pixel horizon crawl while exact anchors and camera-relative transforms preserve precision at
large coordinates. Geometric waves, rain ripples, profile reflection/scattering/absorption,
Fresnel, crest foam, and fog use the same environment buffer as voxel water.

## Vegetation

`VegetationRenderer` loads the validated species registry from the production cooked store and
turns deterministic patches into retained scene objects. Matching plant primitives are emitted by
`SceneRenderSystem` as one hardware-instanced draw rather than one draw per plant.

Each GPU instance carries its stable wind phase, species stiffness, foliage transmission, atlas
frame, and dither visibility. The shared environment wind vector drives foliage and its shadow
vertex program with the same time and phase, preventing detached shadows. LODs overlap through
stable dithered transitions, may reduce density deterministically, and stop casting beyond the
species' shadow LOD. Frustum/distance culling happens in the retained scene; callers may also
submit conservative camera-relative occluder bounds for patch-level visibility rejection.

See [vegetation authoring](../authoring/vegetation.md) for the data format.

## Weather and environmental effects

`WeatherEffects` maps the evaluated precipitation type and intensity to bounded, deterministic
particle events inside a camera-following volume. Rain, snow, ash, and spores share the profile
wind vector with vegetation, water, smoke, and other particles. Emission uses a stable seed and
serial rather than global random state, so frame-equivalent inputs produce equivalent effects even
at large coordinates. Prototype and system budgets cap bursts without allocating in the frame
loop.

The production effects path also provides:

- textured billboard and mesh particles with flipbooks, lit/unlit/emissive shading, three blend
  modes, velocity alignment, local/world simulation, wind, LODs, pooling, and optional depth or
  voxel collision queries;
- soft-particle fading against a graph-owned copy of scene depth;
- generation-safe, camera-facing trail ribbons with bounded segment lifetime and capacity;
- instanced surface marks for soot, mud, wetness, moss, cracks, mining marks, footprints, wheel
  tracks, combat marks, and magical residue.

Surface marks are the renderer's equivalent of projected decals for authored voxel and mesh
surfaces: gameplay supplies an exact point and normal, and presentation owns offset, atlas
variation, fading, material grouping, pooling, and distance limits. See
[environment effect authoring](../authoring/environment_effects.md).

## Runtime integration

The development game combines the selected profile with the deterministic day/night solar path.
Profile sun intensity is modulated by solar elevation, while authored ambient, fog, wind, weather,
water, and exposure remain data-driven. Entering the swimming controller state selects the
underwater profile and applies underwater fog and grading through the same renderer API.

The `starting-biome` benchmark composes the same production paths into a verdant rolling scene:
voxel terrain and river water, instanced meadow/forest/crop vegetation, a large-water body,
profile-driven rain, smoke and embers, and a shadowed fire light. It is intended as the stable
environment integration and performance workload rather than a separate showcase renderer.
