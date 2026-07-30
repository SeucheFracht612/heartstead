#include "engine/renderer/rhi/render_device.hpp"
#include "game/presentation/animated_model_presentation.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace {

heartstead::assets::ModelAnimationClip make_translation_clip(std::string name,
                                                             heartstead::math::Vec3f end) {
    using namespace heartstead;
    assets::ModelAnimationClip clip;
    clip.name = std::move(name);
    clip.duration_seconds = 1.0F;
    clip.channels.push_back({
        1,
        assets::ModelAnimationPath::translation,
        assets::ModelAnimationInterpolation::linear,
        {0.0F, 1.0F},
        {{0.0F, 0.0F, 0.0F, 0.0F}, {end.x, end.y, end.z, 0.0F}},
    });
    return clip;
}

heartstead::assets::ModelAsset make_animated_model() {
    using namespace heartstead;
    assets::ModelAsset model;
    model.vertices = {
        {{-0.25F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
        {{0.25F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
        {{0.0F, 0.75F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
    };
    model.indices = {0, 1, 2};
    model.nodes = {
        {"mesh", assets::no_model_index, {}},
        {"joint", assets::no_model_index, {}},
    };
    model.primitives = {{"body", 0, 3, 0, 3, 0, 0, 0}};
    model.images = {{"body_color",
                     2,
                     2,
                     {255, 64, 32, 255, 32, 255, 64, 255, 32, 64, 255, 255, 255, 255, 255, 0}}};
    assets::ModelMaterial material;
    material.name = "body_material";
    material.base_color_texture.image = 0;
    material.alpha_mode = assets::ModelAlphaMode::mask;
    material.alpha_cutoff = 0.25F;
    material.double_sided = true;
    model.materials.push_back(material);
    model.skins = {{"body_skin", 1, {1}, {math::Mat4f::identity()}}};
    model.animations = {
        make_translation_clip("idle", {}),
        make_translation_clip("walk", {0.0F, 0.25F, 0.0F}),
        make_translation_clip("swim", {0.0F, 0.0F, 0.25F}),
    };
    model.bounds = {{-0.25F, 0.0F, 0.0F}, {0.25F, 0.75F, 0.0F}};
    assert(assets::validate_model_asset(model));
    return model;
}

heartstead::assets::ModelAsset make_rigid_animated_model() {
    using namespace heartstead;
    auto model = make_animated_model();
    model.primitives[0].node = 1;
    model.primitives[0].skin = assets::no_model_index;
    model.skins.clear();
    assert(assets::validate_model_asset(model));
    const auto capabilities = assets::model_capabilities(model);
    assert(capabilities.has_animation_clips);
    assert(capabilities.has_animated_nodes);
    assert(!capabilities.has_skins);
    return model;
}

void initialize_renderer(heartstead::renderer::Renderer& renderer) {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    device_desc.initial_extent = {640, 360};
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const std::vector<std::uint32_t> test_spirv{0x07230203, 0x00010000, 0, 1, 0};
    renderer::RendererInitDesc init;
    init.device = std::move(device).value();
    init.sky_vertex_spirv = test_spirv;
    init.sky_fragment_spirv = test_spirv;
    init.terrain_vertex_spirv = test_spirv;
    init.terrain_fragment_spirv = test_spirv;
    init.static_mesh_vertex_spirv = test_spirv;
    init.static_mesh_fragment_spirv = test_spirv;
    init.debug_vertex_spirv = test_spirv;
    init.debug_fragment_spirv = test_spirv;
    init.ui_vertex_spirv = test_spirv;
    init.ui_fragment_spirv = test_spirv;
    init.tone_map_vertex_spirv = test_spirv;
    init.tone_map_fragment_spirv = test_spirv;
    assert(renderer.initialize(std::move(init)));
}

} // namespace

int main() {
    using namespace heartstead;
    renderer::Renderer renderer;
    initialize_renderer(renderer);
    const auto shared_model = std::make_shared<assets::ModelAsset>(make_animated_model());
    game::AnimatedModelPresentationConfig config;
    config.asset_id = "base:models/entities/test_player.gltf";
    config.visual_prototype = *core::PrototypeId::parse("base:entities/player");
    config.model = shared_model;
    config.locomotion_clips = {
        .idle = 0,
        .walk = 1,
        .run = 1,
        .jump = 1,
        .fall = 0,
        .swim = 2,
        .transition_ticks = 6,
    };
    config.animated_bounds = config.model->bounds.expanded(0.5F);
    auto invalid_scale_config = config;
    invalid_scale_config.model_scale = 0.0F;
    assert(!invalid_scale_config.validate());
    config.model_scale = 0.5F;
    auto second_config = config;
    second_config.visual_prototype = *core::PrototypeId::parse("base:entities/test_animal");

    game::AnimatedModelPresentation presentation;
    assert(presentation.initialize(renderer, std::move(config)));
    game::AnimatedModelPresentation second_presentation;
    assert(second_presentation.initialize(renderer, std::move(second_config)));

    game::RenderSnapshot snapshot;
    snapshot.simulation_tick = 10;
    game::RenderObjectSnapshot player;
    player.id = game::PresentationObjectId::from_parts(1, 1);
    player.source_net_id = core::NetId::from_value(44);
    player.visual_prototype = *core::PrototypeId::parse("base:entities/player");
    player.current_transform.position = world::WorldPosition{0.0, 0.0, -5.0};
    player.previous_transform = player.current_transform;
    player.source_revision = 1;
    snapshot.objects.push_back(player);

    auto inserted = presentation.synchronize(renderer, snapshot);
    assert(inserted);
    assert(inserted.value().inserted_entities == 1);
    assert(inserted.value().retained_entities == 1);
    assert(inserted.value().retained_primitives == 1);
    assert(inserted.value().uploaded_palettes == 1);

    renderer::RenderCamera camera;
    assert(camera.set_aspect_ratio(640.0F / 360.0F));
    auto first_frame = renderer.render(camera);
    assert(first_frame);
    assert(renderer.stats().retained_objects == 1);
    assert(renderer.stats().retained_skin_palettes == 1);
    assert(renderer.stats().submitted_skin_matrices == 1);
    assert(renderer.stats().resident_static_meshes == 2); // fallback plus one shared primitive

    assert(second_presentation.shutdown(renderer));
    assert(renderer.render(camera));
    assert(renderer.stats().retained_objects == 1);
    assert(renderer.stats().resident_static_meshes == 2);

    snapshot.simulation_tick = 12;
    snapshot.objects.front().source_revision = 2;
    snapshot.objects.front().previous_transform = snapshot.objects.front().current_transform;
    snapshot.objects.front().current_transform.position = world::WorldPosition{1.0, 0.0, -5.0};
    snapshot.objects.front().current_locomotion.kind = animation::LocomotionAnimationKind::walk;
    snapshot.objects.front().current_locomotion.transition_from =
        animation::LocomotionAnimationKind::idle;
    snapshot.objects.front().current_locomotion.transition_tick = 10;
    snapshot.objects.front().current_locomotion.phase = 32'768;
    auto updated = presentation.synchronize(renderer, snapshot);
    assert(updated);
    assert(updated.value().updated_entities == 1);
    assert(updated.value().evaluated_poses == 1);
    assert(updated.value().uploaded_palettes == 1);

    snapshot.objects.front().visible = false;
    auto removed = presentation.synchronize(renderer, snapshot);
    assert(removed);
    assert(removed.value().removed_entities == 1);
    assert(removed.value().retained_entities == 0);
    auto empty_frame = renderer.render(camera);
    assert(empty_frame);
    assert(renderer.stats().retained_objects == 0);
    assert(renderer.stats().retained_skin_palettes == 0);

    snapshot.objects.front().visible = true;
    auto restored = presentation.synchronize(renderer, snapshot);
    assert(restored);
    assert(restored.value().inserted_entities == 1);
    assert(restored.value().retained_entities == 1);
    assert(renderer.render(camera));
    assert(renderer.stats().retained_objects == 1);
    assert(renderer.stats().retained_skin_palettes == 1);

    assert(presentation.shutdown(renderer));
    assert(renderer.render(camera));
    assert(renderer.stats().resident_static_meshes == 1);
    assert(renderer.shutdown());

    renderer::Renderer rigid_renderer;
    initialize_renderer(rigid_renderer);
    game::AnimatedModelPresentationConfig rigid_config;
    rigid_config.asset_id = "base:models/entities/rigid_player.glb";
    rigid_config.visual_prototype = *core::PrototypeId::parse("base:entities/rigid_player");
    rigid_config.model = std::make_shared<assets::ModelAsset>(make_rigid_animated_model());
    rigid_config.locomotion_clips = {
        .idle = 0,
        .walk = 1,
        .run = 1,
        .jump = 1,
        .fall = 0,
        .swim = 2,
        .transition_ticks = 6,
    };
    rigid_config.animated_bounds = rigid_config.model->bounds.expanded(0.5F);
    rigid_config.bounds_padding = 0.5F;

    auto no_clip_model = make_rigid_animated_model();
    no_clip_model.animations.clear();
    auto no_clip_config = rigid_config;
    no_clip_config.model = std::make_shared<assets::ModelAsset>(std::move(no_clip_model));
    no_clip_config.animated_bounds = no_clip_config.model->bounds.expanded(0.5F);
    assert(!no_clip_config.validate());

    game::AnimatedModelPresentation rigid_presentation;
    assert(rigid_presentation.initialize(rigid_renderer, std::move(rigid_config)));
    game::RenderSnapshot rigid_snapshot;
    rigid_snapshot.simulation_tick = 10;
    auto rigid_player = player;
    rigid_player.source_net_id = core::NetId::from_value(45);
    rigid_player.visual_prototype = *core::PrototypeId::parse("base:entities/rigid_player");
    rigid_snapshot.objects.push_back(rigid_player);
    auto rigid_inserted = rigid_presentation.synchronize(rigid_renderer, rigid_snapshot);
    assert(rigid_inserted);
    assert(rigid_inserted.value().inserted_entities == 1);
    assert(rigid_inserted.value().evaluated_poses == 1);
    assert(rigid_inserted.value().uploaded_palettes == 0);
    assert(rigid_renderer.render(camera));
    assert(rigid_renderer.stats().retained_objects == 1);
    assert(rigid_renderer.stats().retained_skin_palettes == 0);
    assert(rigid_renderer.stats().submitted_skin_matrices == 0);

    rigid_snapshot.simulation_tick = 12;
    rigid_snapshot.objects.front().source_revision = 2;
    rigid_snapshot.objects.front().current_locomotion.kind =
        animation::LocomotionAnimationKind::walk;
    rigid_snapshot.objects.front().current_locomotion.transition_from =
        animation::LocomotionAnimationKind::idle;
    rigid_snapshot.objects.front().current_locomotion.transition_tick = 10;
    rigid_snapshot.objects.front().current_locomotion.phase = 32'768;
    auto rigid_updated = rigid_presentation.synchronize(rigid_renderer, rigid_snapshot);
    assert(rigid_updated);
    assert(rigid_updated.value().updated_entities == 1);
    assert(rigid_updated.value().evaluated_poses == 1);
    assert(rigid_updated.value().uploaded_palettes == 0);

    assert(rigid_presentation.shutdown(rigid_renderer));
    assert(rigid_renderer.render(camera));
    assert(rigid_renderer.stats().retained_objects == 0);
    assert(rigid_renderer.shutdown());
    return 0;
}
