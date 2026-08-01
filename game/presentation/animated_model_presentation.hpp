#pragma once

#include "engine/animation/animation_budget.hpp"
#include "engine/animation/locomotion_animation.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/core/ids.hpp"
#include "engine/core/result.hpp"
#include "engine/renderer/renderer.hpp"
#include "game/presentation/render_snapshot.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace heartstead::game {

struct StateAnimationClipBinding {
    std::string channel;
    std::string value;
    std::uint32_t clip{assets::no_model_index};
    std::int32_t priority{0};
};

struct AnimatedModelPresentationConfig {
    std::string asset_id;
    core::PrototypeId visual_prototype;
    std::shared_ptr<const assets::ModelAsset> model;
    animation::LocomotionClipSet locomotion_clips;
    std::unordered_map<std::string, std::uint32_t> animation_clips;
    std::unordered_map<std::string, animation::AnimationNodeMask> animation_masks;
    std::unordered_map<std::string, std::vector<animation::AnimationEventMarker>> animation_events;
    std::vector<StateAnimationClipBinding> state_animation_clips;
    std::uint32_t state_transition_ticks{0};
    std::function<void(core::NetId, std::string_view)> animation_event_sink;
    std::function<void(core::NetId, std::span<const math::Mat4f>)> pose_sink;
    std::function<core::Result<math::Mat4f>(const RenderObjectSnapshot&)>
        instance_model_transform;
    std::function<core::Result<std::vector<math::Mat4f>>(const RenderObjectSnapshot&)>
        node_matrices_override;
    renderer::MaterialRuntimeHandle material;
    renderer::RenderLayer layer = renderer::RenderLayer::opaque;
    renderer::RenderObjectFlags flags = renderer::RenderObjectFlags::cast_shadow;
    math::Bounds3f animated_bounds{};
    std::function<bool(const RenderObjectSnapshot&)> object_filter;
    std::function<bool(const RenderObjectSnapshot&, std::string_view)> model_node_visibility;
    std::function<float(const RenderObjectSnapshot&)> animation_distance_squared;
    std::unordered_set<std::string> hidden_model_nodes;
    float model_scale = 1.0F;
    float bounds_padding = 0.25F;
    float minimum_view_distance = 0.0F;
    float maximum_view_distance = 0.0F;
    bool ignore_horizontal_root_motion = true;
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    animation::AnimationBudgetSettings animation_budget{};

    [[nodiscard]] core::Status validate() const;
};

struct AnimatedModelPresentationStats {
    std::uint32_t retained_entities = 0;
    std::uint32_t retained_primitives = 0;
    std::uint32_t evaluated_poses = 0;
    std::uint32_t deferred_pose_evaluations = 0;
    std::uint32_t interpolated_poses = 0;
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
        std::uint64_t last_evaluated_tick = 0;
        animation::NodePose retained_pose;
        std::vector<math::Mat4f> retained_node_matrices;
        std::unordered_map<std::string, float> retained_layer_phases;
        std::string retained_state_animation;
        animation::NodePose state_transition_source;
        std::uint64_t state_transition_start_tick{0};
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
