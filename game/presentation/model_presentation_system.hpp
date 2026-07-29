#pragma once

#include "engine/entities/entity_visual.hpp"
#include "engine/renderer/renderer.hpp"
#include "game/presentation/animated_model_presentation.hpp"
#include "game/presentation/render_snapshot.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace heartstead::game {

struct ModelPresentationSystemStats {
    std::uint32_t definition_count = 0;
    std::uint32_t loaded_model_count = 0;
    AnimatedModelPresentationStats models;
};

class ModelPresentationSystem final {
  public:
    [[nodiscard]] core::Status
    initialize(renderer::Renderer& renderer,
               const entities::VisualDefinitionRegistry& visual_definitions,
               const std::filesystem::path& cooked_asset_root);
    [[nodiscard]] core::Result<ModelPresentationSystemStats>
    synchronize(renderer::Renderer& renderer, const RenderSnapshot& snapshot);
    [[nodiscard]] core::Status shutdown(renderer::Renderer& renderer);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const ModelPresentationSystemStats& stats() const noexcept;

  private:
    struct PresentationEntry {
        core::PrototypeId visual_id;
        AnimatedModelPresentation presentation;
    };

    std::vector<PresentationEntry> presentations_;
    ModelPresentationSystemStats stats_{};
    bool initialized_ = false;
};

} // namespace heartstead::game
