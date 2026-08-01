# Renderer V2 Known Limitations

* UI text covers Western European, Greek, and Cyrillic scripts but lacks complex HarfBuzz shaping and
  fallback for Arabic, Indic, and CJK localization.
* FXAA owns no temporal history. The graph currently exposes transient and external lifetimes;
  explicit cross-frame history allocation belongs with the first temporal effect.
* Generic residency handles prioritized background resources; ordinary texture arrays and mesh arenas
  retain specialized managers.
* Conservative occlusion uses the renderer hierarchy and software depth hierarchy. Vegetation is
  indirect/GPU-driven, while general scene submission intentionally remains hybrid.
* Water covers flowing inland/large water and underwater environment integration; spectral oceans and
  planar reflection probes are optional future quality improvements.
* Vulkan baselines are driver-sensitive. CI must standardize its baseline GPU or software driver.
* Quality profiles directly configure frame images/post effects, shadows, terrain distance, and
  residency. Vegetation, water, particle, reflection, and asset-LOD policy fields are not yet wired
  to live game reconfiguration, and no player-facing selector exists.
* The packaged Noto Sans SFNT is staged beside renderer applications and its SDF atlas is generated
  during renderer initialization rather than loaded from a versioned cooked atlas.
