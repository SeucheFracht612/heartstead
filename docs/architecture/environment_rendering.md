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

## Water

Voxel fluid state remains authoritative for surface height, falling state, and flow direction. The
chunk mesher produces sloped continuous surfaces and flow-oriented UVs. The fluid shader combines
two animated normal samples, rain ripples, angle- and fluid-depth-aware absorption, environment
refraction/reflection, Fresnel, scattering, slope/edge/falling foam, height fog, and profile-driven
underwater grading. All animation uses the renderer environment clock and therefore survives
floating-origin shifts without texture swimming.

Large bodies reuse chunk visibility, meshing, and distance culling, avoiding a separate
large-coordinate coordinate system. The environment profile exposes a large-water LOD distance for
future geometric ocean rings; ordinary voxel oceans already retain stable horizons because their
vertices remain chunk-local and camera-relative.

## Runtime integration

The development game combines the selected profile with the deterministic day/night solar path.
Profile sun intensity is modulated by solar elevation, while authored ambient, fog, wind, weather,
water, and exposure remain data-driven. Entering the swimming controller state selects the
underwater profile and applies underwater fog and grading through the same renderer API.
