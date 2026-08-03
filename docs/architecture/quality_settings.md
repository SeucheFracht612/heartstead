# Renderer Quality Settings

Status: authoritative for Renderer V2.

Player-facing tiers are `Low`, `Medium`, `High`, and `Ultra`. They resolve into explicit values; a
subsystem never branches on the marketing name.

| Control | Low | Medium | High | Ultra |
| --- | ---: | ---: | ---: | ---: |
| Terrain shading | Simplified | Full | Full | Full |
| Internal scale | 50% | 85% | 100% | 100% |
| Anti-aliasing | Off | FXAA | FXAA | FXAA |
| Directional cascades | 1 | 4 | 4 | 4 |
| Directional shadow resolution | 256 | 1024 | 2048 | 4096 |
| Shadow distance | 48 m | 240 m | 320 m | 480 m |
| Local shadow maps | 0 | 1 | 2 | 2 |
| Local shadow resolution | Disabled | 1024 | 1024 | 1024 |
| AO | Off | Low | High | Ultra |
| Bloom | Off | On | On | On |
| Vegetation density | 55% | 75% | 100% | 125% |
| Terrain radius | 8 | 12 | 16 | 24 |
| Particle budget | 50% | 75% | 100% | 150% |
| Texture/residency | 256 MiB | 384 MiB | 512 MiB | 768 MiB |

Settings also carry indirect-light, volumetric, water, reflection, vegetation-distance, particle,
and asset-LOD controls. Renderer initialization directly applies frame scale/post passes, shadows,
terrain/far-terrain distance and memory, and generic residency. The remaining fields are validated
policy values for game-owned presentation orchestration; the development game does not yet wire
them into live vegetation, water, particle, reflection, or asset-LOD reconfiguration.

Simplified terrain shading is a separately compiled fragment path, not a uniform branch through the
full shader. It retains stable infinite-world texture mapping and variants, alpha testing, voxel
light/AO, one-tap cascaded directional shadows, simple fluid animation, fog, HDR output, and core
debug views. It omits normal and surface maps, procedural surface layers, PBR/environment probes,
clustered local lights, multi-tap shadow filtering, and weather material modulation. Medium through
Ultra keep the full authored material model.

Low renders one 256-pixel directional cascade out to 48 metres and disables local shadow maps. The
fixed descriptor and pass-index contract remains intact: inactive directional/local shadow images
are one-pixel depth placeholders, while inactive passes stop reopening those images once they are
initialized. Medium through Ultra retain four cascades and their previous local-shadow resolution.

The development game selects High through `GameApplicationConfig`. There is no command-line or
in-game selector yet, so compare all four resolved profiles with
`heartstead_renderer_quality_tests` rather than assuming a UI control exists.

Tone mapping and UI stay at output resolution and share one dynamic-rendering scope only while they
remain consecutive users of the same attachment. Disabled bloom uses a one-pixel black input;
disabled AO and FXAA are graph bypasses, not identity shader draws. These optimizations preserve the
stable 19-pass index and descriptor contracts.
