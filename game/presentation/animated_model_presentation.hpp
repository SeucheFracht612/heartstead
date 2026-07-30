#pragma once

#include "engine/animation/locomotion_animation.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/renderer.hpp"
#include "game/presentation/render_snapshot.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace heartstead::game {

struct AnimatedModelPresentationConfig {
    std::string asset_id;
    core::PrototypeId visual_prototype;
    std::shared_ptr<const assets::ModelAsset> model;
    animation::LocomotionClipSet locomotion_clips;
    renderer::MaterialRuntimeHandle material;
    renderer::RenderLayer layer = renderer::RenderLayer::opaque;
    renderer::RenderObjectFlags flags = renderer::RenderObjectFlags::cast_shadow;
    math::Bounds3f animated_bounds{};
    float model_scale = 1.0F;
    float bounds_padding = 0.25F;
    bool ignore_horizontal_root_motion = true;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};

    [[nodiscard]] core::Status validate() const;
};

struct AnimatedModelPresentationStats {
    std::uint32_t retained_entities = 0;
    std::uint32_t retained_primitives = 0;
    std::uint32_t evaluated_poses = 0;
    std::uint32_t uploaded_palettes = 0;
    std::uint32_t inserted_entities = 0;
    std::uint32_t updated_entities = 0;
    std::uint32_t removed_entities = 0;
};

class AnimatedModelPresentation final {
  public:
    [[nodiscard]] core::Status initialize(renderer::Renderer& renderer,
                                          AnimatedModelPresentationConfig config);
    [[nodiscard]] core::Result<AnimatedModelPresentationStats>
    synchronize(renderer::Renderer& renderer, const RenderSnapshot& snapshot);
    [[nodiscard]] core::Status shutdown(renderer::Renderer& renderer);

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] const AnimatedModelPresentationStats& stats() const noexcept;

  private:
    struct PrimitiveBinding {
        std::uint32_t primitive_index = 0;
        renderer::RenderMeshHandle mesh;
        renderer::MaterialRuntimeHandle material;
        renderer::RenderLayer layer = renderer::RenderLayer::opaque;
        renderer::RenderObjectFlags flags = renderer::RenderObjectFlags::none;
        math::Bounds3f local_bounds{};
    };

    struct PrimitiveVisual {
        renderer::RenderObjectId object;
        renderer::RenderSkinPaletteId palette;
    };

    struct EntityVisual {
        std::uint64_t source_revision = 0;
        std::vector<PrimitiveVisual> primitives;
    };

    AnimatedModelPresentationConfig config_;
    std::vector<PrimitiveBinding> primitives_;
    animation::NodePose bind_pose_;
    std::vector<math::Mat4f> bind_node_matrices_;
    math::Mat4f model_scale_matrix_ = math::Mat4f::identity();
    std::unordered_map<std::uint64_t, EntityVisual> entities_;
    AnimatedModelPresentationStats stats_;
    bool has_active_animation_ = false;
    bool initialized_ = false;
};

} // namespace heartstead::game
