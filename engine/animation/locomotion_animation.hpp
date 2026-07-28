#pragma once

#include "engine/animation/skeletal_animation.hpp"

#include <cstdint>
#include <string_view>

namespace heartstead::animation {

enum class LocomotionAnimationKind : std::uint8_t {
    idle,
    walk,
    swim,
};

struct ReplicatedLocomotionAnimation {
    LocomotionAnimationKind kind = LocomotionAnimationKind::idle;
    std::uint16_t phase = 0;
    LocomotionAnimationKind transition_from = LocomotionAnimationKind::idle;
    std::uint16_t transition_from_phase = 0;
    std::uint64_t transition_tick = 0;

    [[nodiscard]] core::Status validate(std::uint64_t simulation_tick) const;
    [[nodiscard]] float normalized_phase() const noexcept;
    [[nodiscard]] float normalized_transition_from_phase() const noexcept;

    friend bool operator==(const ReplicatedLocomotionAnimation&,
                           const ReplicatedLocomotionAnimation&) = default;
};

struct LocomotionClipSet {
    std::uint32_t idle = assets::no_model_index;
    std::uint32_t walk = assets::no_model_index;
    std::uint32_t swim = assets::no_model_index;
    std::uint32_t transition_ticks = 9;

    [[nodiscard]] core::Status validate(const assets::ModelAsset& model) const;
    [[nodiscard]] std::uint32_t clip_for(LocomotionAnimationKind kind) const noexcept;
};

[[nodiscard]] core::Result<SkeletalPose>
sample_locomotion_animation(const assets::ModelAsset& model, const LocomotionClipSet& clips,
                            const ReplicatedLocomotionAnimation& animation,
                            std::uint64_t render_tick);
[[nodiscard]] std::string_view
locomotion_animation_kind_name(LocomotionAnimationKind kind) noexcept;

} // namespace heartstead::animation
