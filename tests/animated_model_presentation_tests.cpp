#include "engine/renderer/rhi/render_device.hpp"
#include "game/presentation/animated_model_presentation.hpp"

#include <cassert>
#include <cstdint>
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
    material.base_color_image = 0;
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
    assert(renderer.initialize(std::move(init)));
}

} // namespace

int main() {
    using namespace heartstead;
    renderer::Renderer renderer;
    initialize_renderer(renderer);
    game::AnimatedModelPresentationConfig config;
    config.asset_id = "base:models/entities/test_player.gltf";
    config.visual_prototype = *core::PrototypeId::parse("base:entities/player");
    config.model = make_animated_model();
    config.locomotion_clips = {
        .idle = 0,
        .walk = 1,
        .run = 1,
        .jump = 1,
        .fall = 0,
        .swim = 2,
        .transition_ticks = 6,
    };
    config.animated_bounds = config.model.bounds.expanded(0.5F);

    game::AnimatedModelPresentation presentation;
    assert(presentation.initialize(renderer, std::move(config)));

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

    snapshot.objects.clear();
    auto removed = presentation.synchronize(renderer, snapshot);
    assert(removed);
    assert(removed.value().removed_entities == 1);
    assert(removed.value().retained_entities == 0);
    auto empty_frame = renderer.render(camera);
    assert(empty_frame);
    assert(renderer.stats().retained_objects == 0);
    assert(renderer.stats().retained_skin_palettes == 0);

    assert(presentation.shutdown(renderer));
    assert(renderer.shutdown());
    return 0;
}
