#pragma once

#include "engine/entities/entity_visual.hpp"
#include "engine/renderer/renderer.hpp"
#include "game/presentation/animated_model_presentation.hpp"
#include "game/presentation/render_snapshot.hpp"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace heartstead::game {

struct ModelPresentationSystemConfig {
    std::string fallback_visual_id = "base:visuals/fallback";
    std::string fallback_animation_role = "idle";
};

struct PresentationAssetLoadDiagnostic {
    std::string logical_id;
    std::string source_path;
    std::string cooked_path;
    std::string failing_dependency;
    std::string fallback_used;
    std::string error_code;
    std::string message;
};

struct ModelPresentationSystemStats {
    std::uint32_t definition_count = 0;
    std::uint32_t loaded_model_count = 0;
    std::uint32_t fallback_entity_count = 0;
    std::uint32_t unresolved_visual_count = 0;
    std::uint32_t fallback_model_definition_count = 0;
    std::uint32_t fallback_animation_mapping_count = 0;
    std::uint32_t load_diagnostic_count = 0;
    AnimatedModelPresentationStats models;
};

class ModelPresentationSystem final {
  public:
    [[nodiscard]] core::Status initialize(
        renderer::Renderer& renderer, const entities::VisualDefinitionRegistry& visual_definitions,
        const std::filesystem::path& cooked_asset_root, ModelPresentationSystemConfig config = {});
    [[nodiscard]] core::Result<ModelPresentationSystemStats>
    synchronize(renderer::Renderer& renderer, const RenderSnapshot& snapshot);
    [[nodiscard]] core::Status shutdown(renderer::Renderer& renderer);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const ModelPresentationSystemStats& stats() const noexcept;
    [[nodiscard]] std::span<const PresentationAssetLoadDiagnostic> load_diagnostics() const noexcept;

  private:
    struct PresentationEntry {
        core::PrototypeId visual_id;
        bool is_fallback = false;
        AnimatedModelPresentation presentation;
    };

    std::vector<PresentationEntry> presentations_;
    std::unordered_set<std::string> known_visual_prototypes_;
    std::unordered_set<std::string> unresolved_visuals_;
    std::vector<PresentationAssetLoadDiagnostic> load_diagnostics_;
    core::PrototypeId fallback_visual_prototype_;
    ModelPresentationSystemStats stats_{};
    bool initialized_ = false;
};

} // namespace heartstead::game
