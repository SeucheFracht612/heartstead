#include "engine/animation/locomotion_animation.hpp"

#include <algorithm>

namespace heartstead::animation {

namespace {

constexpr float phase_denominator = 65'536.0F;

[[nodiscard]] bool valid_kind(LocomotionAnimationKind kind) noexcept {
    switch (kind) {
    case LocomotionAnimationKind::idle:
    case LocomotionAnimationKind::walk:
    case LocomotionAnimationKind::swim:
        return true;
    }
    return false;
}

[[nodiscard]] AnimationClipPlayback playback_for(const assets::ModelAsset& model,
                                                 std::uint32_t clip, float normalized_phase) {
    return {clip, model.animations[clip].duration_seconds * normalized_phase, false};
}

} // namespace

core::Status ReplicatedLocomotionAnimation::validate(std::uint64_t simulation_tick) const {
    if (!valid_kind(kind) || !valid_kind(transition_from) || transition_tick > simulation_tick) {
        return core::Status::failure(
            "locomotion_animation.invalid_state",
            "replicated locomotion animation state or transition tick is invalid");
    }
    return core::Status::ok();
}

float ReplicatedLocomotionAnimation::normalized_phase() const noexcept {
    return static_cast<float>(phase) / phase_denominator;
}

float ReplicatedLocomotionAnimation::normalized_transition_from_phase() const noexcept {
    return static_cast<float>(transition_from_phase) / phase_denominator;
}

core::Status LocomotionClipSet::validate(const assets::ModelAsset& model) const {
    if (idle >= model.animations.size() || walk >= model.animations.size() ||
        swim >= model.animations.size() || transition_ticks == 0 || transition_ticks > 600) {
        return core::Status::failure(
            "locomotion_animation.invalid_clips",
            "locomotion clip set requires valid idle/walk/swim clips and a bounded transition");
    }
    return core::Status::ok();
}

std::uint32_t LocomotionClipSet::clip_for(LocomotionAnimationKind kind) const noexcept {
    switch (kind) {
    case LocomotionAnimationKind::idle:
        return idle;
    case LocomotionAnimationKind::walk:
        return walk;
    case LocomotionAnimationKind::swim:
        return swim;
    }
    return assets::no_model_index;
}

core::Result<SkeletalPose>
sample_locomotion_animation(const assets::ModelAsset& model, const LocomotionClipSet& clips,
                            const ReplicatedLocomotionAnimation& animation,
                            std::uint64_t render_tick) {
    auto status = clips.validate(model);
    if (!status) {
        return core::Result<SkeletalPose>::failure(status.error().code, status.error().message);
    }
    status = animation.validate(std::max(render_tick, animation.transition_tick));
    if (!status) {
        return core::Result<SkeletalPose>::failure(status.error().code, status.error().message);
    }
    const auto target_clip = clips.clip_for(animation.kind);
    auto target_pose = sample_animation_clip(
        model, playback_for(model, target_clip, animation.normalized_phase()));
    if (!target_pose) {
        return target_pose;
    }
    if (animation.transition_tick == 0 || animation.transition_from == animation.kind ||
        (render_tick >= animation.transition_tick &&
         render_tick - animation.transition_tick >= clips.transition_ticks)) {
        return target_pose;
    }
    const auto source_clip = clips.clip_for(animation.transition_from);
    auto source_pose = sample_animation_clip(
        model, playback_for(model, source_clip, animation.normalized_transition_from_phase()));
    if (!source_pose) {
        return source_pose;
    }
    const auto elapsed = render_tick > animation.transition_tick
                             ? std::min<std::uint64_t>(render_tick - animation.transition_tick,
                                                       clips.transition_ticks)
                             : 0;
    const auto weight = static_cast<float>(elapsed) / static_cast<float>(clips.transition_ticks);
    return blend_skeletal_poses(model, source_pose.value(), target_pose.value(), weight);
}

std::string_view locomotion_animation_kind_name(LocomotionAnimationKind kind) noexcept {
    switch (kind) {
    case LocomotionAnimationKind::idle:
        return "idle";
    case LocomotionAnimationKind::walk:
        return "walk";
    case LocomotionAnimationKind::swim:
        return "swim";
    }
    return "unknown";
}

} // namespace heartstead::animation
