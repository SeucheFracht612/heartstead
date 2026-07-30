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
    case LocomotionAnimationKind::run:
    case LocomotionAnimationKind::jump:
    case LocomotionAnimationKind::fall:
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
        swim >= model.animations.size() || run >= model.animations.size() ||
        jump >= model.animations.size() || fall >= model.animations.size() ||
        transition_ticks == 0 || transition_ticks > 600) {
        return core::Status::failure(
            "locomotion_animation.invalid_clips",
            "locomotion clip set requires valid idle/walk/run/jump/fall/swim clips and a bounded "
            "transition");
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
    case LocomotionAnimationKind::run:
        return run;
    case LocomotionAnimationKind::jump:
        return jump;
    case LocomotionAnimationKind::fall:
        return fall;
    }
    return assets::no_model_index;
}

core::Result<LocomotionClipSet>
resolve_locomotion_clips(const assets::ModelAsset& model, std::string_view idle,
                         std::string_view walk, std::string_view run, std::string_view jump,
                         std::string_view fall, std::string_view swim,
                         std::uint32_t transition_ticks) {
    auto idle_clip = assets::resolve_model_animation_clip(model, idle);
    if (!idle_clip) {
        return core::Result<LocomotionClipSet>::failure(idle_clip.error().code,
                                                        idle_clip.error().message);
    }
    auto walk_clip = assets::resolve_model_animation_clip(model, walk);
    if (!walk_clip) {
        return core::Result<LocomotionClipSet>::failure(walk_clip.error().code,
                                                        walk_clip.error().message);
    }
    auto swim_clip = assets::resolve_model_animation_clip(model, swim);
    if (!swim_clip) {
        return core::Result<LocomotionClipSet>::failure(swim_clip.error().code,
                                                        swim_clip.error().message);
    }
    auto run_clip = assets::resolve_model_animation_clip(model, run);
    auto jump_clip = assets::resolve_model_animation_clip(model, jump);
    auto fall_clip = assets::resolve_model_animation_clip(model, fall);
    if (!run_clip || !jump_clip || !fall_clip) {
        const auto& error = !run_clip    ? run_clip.error()
                            : !jump_clip ? jump_clip.error()
                                         : fall_clip.error();
        return core::Result<LocomotionClipSet>::failure(error.code, error.message);
    }
    LocomotionClipSet result;
    result.idle = idle_clip.value();
    result.walk = walk_clip.value();
    result.run = run_clip.value();
    result.jump = jump_clip.value();
    result.fall = fall_clip.value();
    result.swim = swim_clip.value();
    result.transition_ticks = transition_ticks;
    auto status = result.validate(model);
    if (!status) {
        return core::Result<LocomotionClipSet>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<LocomotionClipSet>::success(result);
}

core::Result<NodePose>
sample_locomotion_animation(const assets::ModelAsset& model, const LocomotionClipSet& clips,
                            const ReplicatedLocomotionAnimation& animation,
                            std::uint64_t render_tick) {
    auto status = clips.validate(model);
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
    }
    status = animation.validate(std::max(render_tick, animation.transition_tick));
    if (!status) {
        return core::Result<NodePose>::failure(status.error().code, status.error().message);
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
    return blend_node_poses(model, source_pose.value(), target_pose.value(), weight);
}

std::string_view locomotion_animation_kind_name(LocomotionAnimationKind kind) noexcept {
    switch (kind) {
    case LocomotionAnimationKind::idle:
        return "idle";
    case LocomotionAnimationKind::walk:
        return "walk";
    case LocomotionAnimationKind::swim:
        return "swim";
    case LocomotionAnimationKind::run:
        return "run";
    case LocomotionAnimationKind::jump:
        return "jump";
    case LocomotionAnimationKind::fall:
        return "fall";
    }
    return "unknown";
}

} // namespace heartstead::animation
