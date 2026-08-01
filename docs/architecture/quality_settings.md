# Renderer Quality Settings

Status: authoritative for Renderer V2.

Player-facing tiers are `Low`, `Medium`, `High`, and `Ultra`. They resolve into explicit values; a
subsystem never branches on the marketing name.

| Control | Low | Medium | High | Ultra |
| --- | ---: | ---: | ---: | ---: |
| Internal scale | 67% | 85% | 100% | 100% |
| Anti-aliasing | Off | FXAA | FXAA | FXAA |
| Directional shadow | 1024 | 1024 | 2048 | 4096 |
| Shadow distance | 160 m | 240 m | 320 m | 480 m |
| Local shadows | 0 | 1 | 2 | 2 |
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

The development game selects High through `GameApplicationConfig`. There is no command-line or
in-game selector yet, so compare all four resolved profiles with
`heartstead_renderer_quality_tests` rather than assuming a UI control exists.

Tone mapping and UI stay at output resolution. Disabled bloom clears its input black; disabled AO and
FXAA are graph bypasses, not identity shader draws, preserving the stable 19-pass index contract.
