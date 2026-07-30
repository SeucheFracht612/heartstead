#include "engine/animation/locomotion_animation.hpp"
#include "engine/animation/skeletal_animation.hpp"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {

bool nearly_equal(float left, float right, float tolerance = 0.0001F) {
    return std::abs(left - right) <= tolerance;
}

heartstead::assets::ModelAsset make_animated_model() {
    using namespace heartstead;
    assets::ModelAsset model;
    model.vertices.push_back({
        {1.0F, 0.0F, 0.0F},
        {1.0F, 0.0F, 0.0F},
        {},
        {0, 0, 0, 0},
        {1.0F, 0.0F, 0.0F, 0.0F},
    });
    model.indices = {0, 0, 0};
    model.nodes = {
        {"mesh", assets::no_model_index, {}},
        {"joint", 0, {}},
    };
    model.primitives.push_back({"body", 0, 1, 0, 3, 0, 0});
    model.skins.push_back({"body_skin", 0, {1}, {math::Mat4f::identity()}});
    model.bounds = {{1.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}};

    assets::ModelAnimationClip walk;
    walk.name = "walk";
    walk.duration_seconds = 1.0F;
    walk.channels.push_back({
        1,
        assets::ModelAnimationPath::translation,
        assets::ModelAnimationInterpolation::linear,
        {0.0F, 1.0F},
        {{0.0F, 0.0F, 0.0F, 0.0F}, {0.0F, 2.0F, 0.0F, 0.0F}},
    });
    walk.channels.push_back({
        1,
        assets::ModelAnimationPath::rotation,
        assets::ModelAnimationInterpolation::linear,
        {0.0F, 1.0F},
        {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F, 0.0F}},
    });
    model.animations.push_back(std::move(walk));

    assets::ModelAnimationClip cubic;
    cubic.name = "cubic";
    cubic.duration_seconds = 2.0F;
    cubic.channels.push_back({
        1,
        assets::ModelAnimationPath::translation,
        assets::ModelAnimationInterpolation::cubic_spline,
        {0.0F, 2.0F},
        {
            {},
            {},
            {1.0F, 0.0F, 0.0F, 0.0F},
            {-1.0F, 0.0F, 0.0F, 0.0F},
            {},
            {},
        },
    });
    model.animations.push_back(std::move(cubic));

    assets::ModelAnimationClip step;
    step.name = "step";
    step.duration_seconds = 1.0F;
    step.channels.push_back({
        1,
        assets::ModelAnimationPath::scale,
        assets::ModelAnimationInterpolation::step,
        {0.0F, 1.0F},
        {{1.0F, 1.0F, 1.0F, 0.0F}, {2.0F, 2.0F, 2.0F, 0.0F}},
    });
    model.animations.push_back(std::move(step));
    assert(assets::validate_model_asset(model));
    return model;
}

} // namespace

int main() {
    using namespace heartstead;
    const auto model = make_animated_model();
    const auto capabilities = assets::model_capabilities(model);
    assert(capabilities.has_animation_clips);
    assert(capabilities.has_skins);
    assert(!capabilities.has_morph_targets);
    assert(capabilities.has_animated_nodes);
    assert(capabilities.supports_animation());

    const auto bind = animation::bind_node_pose(model);
    assert(bind.local_transforms.size() == model.nodes.size());
    assert(animation::validate_node_pose(model, bind));

    auto half_walk = animation::sample_animation_clip(model, {0, 0.5F, false});
    assert(half_walk);
    const auto& joint = half_walk.value().local_transforms[1];
    assert(nearly_equal(joint.translation.y, 1.0F));
    assert(nearly_equal(std::abs(joint.rotation.z), std::sqrt(0.5F)));
    assert(nearly_equal(std::abs(joint.rotation.w), std::sqrt(0.5F)));

    auto palette = animation::build_skinning_palette(model, 0, 0, half_walk.value());
    assert(palette);
    assert(palette.value().joint_matrices.size() == 1);
    auto skinned =
        animation::skin_model_vertex(model.vertices.front(), palette.value().joint_matrices);
    assert(skinned);
    assert(nearly_equal(skinned.value().position.x, 0.0F));
    assert(nearly_equal(skinned.value().position.y, 2.0F));
    assert(nearly_equal(skinned.value().normal.x, 0.0F));
    assert(nearly_equal(skinned.value().normal.y, 1.0F));

    auto looped = animation::sample_animation_clip(model, {0, 1.25F, true});
    auto unlooped = animation::sample_animation_clip(model, {0, 1.25F, false});
    assert(looped && unlooped);
    assert(nearly_equal(looped.value().local_transforms[1].translation.y, 0.5F));
    assert(nearly_equal(unlooped.value().local_transforms[1].translation.y, 2.0F));

    auto cubic = animation::sample_animation_clip(model, {1, 1.0F, false});
    assert(cubic);
    assert(nearly_equal(cubic.value().local_transforms[1].translation.x, 0.5F));

    auto stepped_before = animation::sample_animation_clip(model, {2, 0.999F, false});
    auto stepped_after = animation::sample_animation_clip(model, {2, 1.0F, false});
    assert(stepped_before && stepped_after);
    assert(nearly_equal(stepped_before.value().local_transforms[1].scale.x, 1.0F));
    assert(nearly_equal(stepped_after.value().local_transforms[1].scale.x, 2.0F));

    auto blended = animation::blend_node_poses(model, bind, half_walk.value(), 0.25F);
    assert(blended);
    assert(nearly_equal(blended.value().local_transforms[1].translation.y, 0.25F));
    auto sampled_blend =
        animation::sample_blended_animation(model, {0, 0.0F, false}, {0, 1.0F, false}, 0.5F);
    assert(sampled_blend);
    assert(nearly_equal(sampled_blend.value().local_transforms[1].translation.y, 1.0F));

    auto bad_clip = animation::sample_animation_clip(model, {assets::no_model_index, 0.0F, true});
    assert(!bad_clip);
    auto bad_blend = animation::blend_node_poses(model, bind, half_walk.value(), 1.01F);
    assert(!bad_blend);
    auto bad_palette = animation::build_skinning_palette(model, 7, 0, half_walk.value());
    assert(!bad_palette);
    auto bad_vertex = model.vertices.front();
    bad_vertex.joints[0] = 2;
    assert(!animation::skin_model_vertex(bad_vertex, palette.value().joint_matrices));

    auto translated_model = model;
    translated_model.nodes[0].bind_transform.translation = {5.0F, 0.0F, 0.0F};
    auto translated_pose = animation::sample_animation_clip(translated_model, {0, 0.5F, false});
    assert(translated_pose);
    auto mesh_local_palette =
        animation::build_skinning_palette(translated_model, 0, 0, translated_pose.value());
    assert(mesh_local_palette);
    auto mesh_local_vertex = animation::skin_model_vertex(
        translated_model.vertices.front(), mesh_local_palette.value().joint_matrices);
    assert(mesh_local_vertex);
    assert(nearly_equal(mesh_local_vertex.value().position.x, 0.0F));
    assert(nearly_equal(mesh_local_vertex.value().position.y, 2.0F));
    auto model_space_palette = animation::build_model_space_skinning_palette(
        translated_model, 0, 0, translated_pose.value());
    assert(model_space_palette);
    auto model_space_vertex = animation::skin_model_vertex(
        translated_model.vertices.front(), model_space_palette.value().joint_matrices);
    assert(model_space_vertex);
    assert(nearly_equal(model_space_vertex.value().position.x, 5.0F));
    assert(nearly_equal(model_space_vertex.value().position.y, 2.0F));

    animation::ReplicatedLocomotionAnimation locomotion;
    locomotion.kind = animation::LocomotionAnimationKind::walk;
    locomotion.phase = 32'768;
    locomotion.transition_from = animation::LocomotionAnimationKind::idle;
    locomotion.transition_tick = 10;
    animation::LocomotionClipSet locomotion_clips;
    locomotion_clips.idle = 2;
    locomotion_clips.walk = 0;
    locomotion_clips.run = 0;
    locomotion_clips.jump = 2;
    locomotion_clips.fall = 1;
    locomotion_clips.swim = 1;
    locomotion_clips.transition_ticks = 5;
    auto transition_start =
        animation::sample_locomotion_animation(model, locomotion_clips, locomotion, 10);
    auto transition_middle =
        animation::sample_locomotion_animation(model, locomotion_clips, locomotion, 12);
    auto transition_end =
        animation::sample_locomotion_animation(model, locomotion_clips, locomotion, 15);
    auto second_client_pose =
        animation::sample_locomotion_animation(model, locomotion_clips, locomotion, 12);
    assert(transition_start && transition_middle && transition_end && second_client_pose);
    assert(transition_middle.value() == second_client_pose.value());
    assert(nearly_equal(transition_start.value().local_transforms[1].translation.y, 0.0F));
    assert(nearly_equal(transition_middle.value().local_transforms[1].translation.y, 0.4F));
    assert(nearly_equal(transition_end.value().local_transforms[1].translation.y, 1.0F));

    auto morph_model = model;
    morph_model.nodes[0].morph_weights = {0.0F};
    assets::ModelMorphTarget morph_target;
    morph_target.position_deltas = {{0.0F, 1.0F, 0.0F}};
    morph_model.primitives[0].morph_targets = {morph_target};
    assets::ModelAnimationClip morph_clip;
    morph_clip.name = "morph";
    morph_clip.duration_seconds = 1.0F;
    morph_clip.channels.emplace_back(0, assets::ModelAnimationPath::weights,
                                     assets::ModelAnimationInterpolation::linear,
                                     std::vector<float>{0.0F, 1.0F}, std::vector<math::Vec4f>{}, 1U,
                                     std::vector<float>{0.0F, 1.0F});
    morph_model.animations.push_back(std::move(morph_clip));
    assert(assets::validate_model_asset(morph_model));
    const auto morph_capabilities = assets::model_capabilities(morph_model);
    assert(morph_capabilities.has_morph_targets);
    auto half_morph = animation::sample_animation_clip(morph_model, {3, 0.5F, false});
    assert(half_morph);
    assert(nearly_equal(half_morph.value().morph_weights[0][0], 0.5F));
    auto blended_morph = animation::blend_node_poses(
        morph_model, animation::bind_node_pose(morph_model), half_morph.value(), 0.5F);
    assert(blended_morph);
    assert(nearly_equal(blended_morph.value().morph_weights[0][0], 0.25F));

    auto rigid_model = model;
    rigid_model.primitives[0].skin = assets::no_model_index;
    rigid_model.skins.clear();
    assert(assets::validate_model_asset(rigid_model));
    const auto rigid_capabilities = assets::model_capabilities(rigid_model);
    assert(rigid_capabilities.has_animation_clips);
    assert(!rigid_capabilities.has_skins);
    assert(rigid_capabilities.has_animated_nodes);
    auto rigid_pose = animation::sample_animation_clip(rigid_model, {0, 0.5F, false});
    assert(rigid_pose);
    auto rigid_matrices =
        animation::evaluate_model_node_matrices(rigid_model, rigid_pose.value());
    assert(rigid_matrices);
    const auto rigid_origin =
        rigid_matrices.value()[1] * math::Vec4f{0.0F, 0.0F, 0.0F, 1.0F};
    assert(nearly_equal(rigid_origin.y, 1.0F));

    return 0;
}
