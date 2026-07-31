#pragma once

#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/entities/entity_visual.hpp"
#include "engine/renderer/lighting/cascaded_shadows.hpp"
#include "game/application/game_application.hpp"
#include "game/presentation/model_presentation_system.hpp"
#include "game/presentation/particle_presentation.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace heartstead::asset_lab {

enum class PreviewKind : std::uint8_t {
    static_model,
    animated_model,
    character,
    equipment,
    terrain_material,
    vegetation,
    particle,
    material,
    texture,
    lod,
    visual_prefab,
};

enum class LightingPreset : std::uint8_t {
    studio,
    overcast,
    noon,
    sunset,
    night,
    fire_lit_interior,
    cave,
    forest_shade,
    rain_wetness,
    snow_frost,
    underwater,
};

struct AssetLabModeConfig {
    const content::ContentValidationReport* content = nullptr;
    std::filesystem::path cooked_asset_root;
    PreviewKind preview = PreviewKind::visual_prefab;
    LightingPreset lighting = LightingPreset::studio;
    renderer::LightingDebugView debug_view = renderer::LightingDebugView::none;
    std::string selection_id;
    std::vector<entities::VisualStateValue> visual_states;
    std::optional<std::uint32_t> forced_lod;
    std::string equipment_asset;
    std::string equipment_socket;
    bool show_bounds = false;
    bool show_skeleton = false;
};

struct AssetLabInspection {
    std::string selection_id;
    std::string selection_kind;
    std::string source_path;
    std::string cooked_path;
    std::string format;
    std::uint64_t runtime_bytes = 0;
    std::vector<std::string> details;

    [[nodiscard]] std::string summary() const;
};

[[nodiscard]] std::optional<PreviewKind> parse_preview_kind(std::string_view value) noexcept;
[[nodiscard]] std::string_view preview_kind_name(PreviewKind value) noexcept;
[[nodiscard]] std::optional<LightingPreset> parse_lighting_preset(std::string_view value) noexcept;
[[nodiscard]] std::string_view lighting_preset_name(LightingPreset value) noexcept;
[[nodiscard]] std::optional<renderer::LightingDebugView>
parse_asset_lab_debug_view(std::string_view value) noexcept;
[[nodiscard]] std::string_view
asset_lab_debug_view_name(renderer::LightingDebugView value) noexcept;
[[nodiscard]] renderer::rhi::RenderEnvironmentData
asset_lab_lighting_environment(LightingPreset preset) noexcept;
[[nodiscard]] core::Result<AssetLabInspection>
inspect_asset_lab_selection(const AssetLabModeConfig& config,
                            const assets::CookedAssetStore& cooked_assets);

class AssetLabMode final : public game::IGameApplicationMode {
  public:
    explicit AssetLabMode(AssetLabModeConfig config);

    [[nodiscard]] core::Status initialize(game::GameApplicationServices& services) override;
    [[nodiscard]] core::Result<game::GameApplicationFrameOutput>
    update(game::GameApplicationServices& services,
           const game::GameApplicationFrame& frame) override;
    [[nodiscard]] core::Status shutdown(game::GameApplicationServices& services) override;
    [[nodiscard]] std::string summary() const override;

  private:
    [[nodiscard]] core::Status initialize_model_preview(renderer::Renderer& renderer);
    [[nodiscard]] core::Status initialize_particle_preview(renderer::Renderer& renderer);

    AssetLabModeConfig config_;
    std::optional<assets::CookedAssetStore> cooked_assets_;
    std::optional<AssetLabInspection> inspection_;
    entities::VisualDefinitionRegistry preview_visuals_;
    game::ModelPresentationSystem models_;
    std::optional<renderer::CpuParticleSystem> particles_;
    game::ParticlePresentation particle_presentation_;
    renderer::RenderCamera camera_;
    game::RenderSnapshot snapshot_;
    core::PrototypeId preview_entity_;
    core::PrototypeId equipment_entity_;
    std::optional<assets::ModelAsset> inspected_model_;
    std::vector<math::Mat4f> inspected_node_matrices_;
    math::Vec3f equipment_offset_{};
    bool models_initialized_ = false;
    bool particles_initialized_ = false;
};

} // namespace heartstead::asset_lab
