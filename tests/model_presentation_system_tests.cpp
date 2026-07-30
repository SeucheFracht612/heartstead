#include "engine/assets/cooked_asset_store.hpp"
#include "engine/assets/model_asset.hpp"
#include "engine/content/content_validation.hpp"
#include "engine/core/logging.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "game/presentation/model_presentation_system.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void initialize_renderer(heartstead::renderer::Renderer& renderer) {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    device_desc.initial_extent = {640, 360};
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const std::vector<std::uint32_t> spirv{0x07230203, 0x00010000, 0, 1, 0};
    renderer::RendererInitDesc init;
    init.device = std::move(device).value();
    init.sky_vertex_spirv = spirv;
    init.sky_fragment_spirv = spirv;
    init.terrain_vertex_spirv = spirv;
    init.terrain_fragment_spirv = spirv;
    init.static_mesh_vertex_spirv = spirv;
    init.static_mesh_fragment_spirv = spirv;
    init.debug_vertex_spirv = spirv;
    init.debug_fragment_spirv = spirv;
    init.ui_vertex_spirv = spirv;
    init.ui_fragment_spirv = spirv;
    init.tone_map_vertex_spirv = spirv;
    init.tone_map_fragment_spirv = spirv;
    assert(renderer.initialize(std::move(init)));
}

heartstead::game::RenderObjectSnapshot make_object(std::uint64_t net_id, std::string_view prototype,
                                                   double x) {
    using namespace heartstead;
    game::RenderObjectSnapshot object;
    object.id = game::PresentationObjectId::from_parts(static_cast<std::uint32_t>(net_id), 1);
    object.source_net_id = core::NetId::from_value(net_id);
    object.visual_prototype = *core::PrototypeId::parse(prototype);
    object.current_transform.position = world::WorldPosition{x, 0.0, -5.0};
    object.previous_transform = object.current_transform;
    object.source_revision = 1;
    return object;
}

} // namespace

int main() {
    using namespace heartstead;
    const auto content =
        content::ContentValidation::validate(std::filesystem::path{HEARTSTEAD_TEST_SOURCE_DIR});
    assert(!content.has_errors());
    assert(content.visual_definitions.size() == 4);
    const auto player = core::PrototypeId::parse("base:entities/player");
    const auto animal = core::PrototypeId::parse("base:entities/test_animal");
    const auto fallback = core::PrototypeId::parse("base:visuals/fallback");
    assert(player && animal && fallback);
    const auto* player_visual = content.visual_definitions.find_for_entity(*player);
    assert(player_visual != nullptr);
    assert(player_visual->model_scale == 0.66F);
    assert(content.visual_definitions.find_for_entity(*animal) != nullptr);
    assert(content.visual_definitions.find(*fallback) != nullptr);

    auto store =
        assets::CookedAssetStore::load(std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR});
    assert(store);
    assert(store.value().manifest().validate_dependencies());
    std::unordered_set<std::string> visual_models;
    std::unordered_map<std::string, std::size_t> visual_primitive_counts;
    std::size_t fallback_primitive_count = 0;
    for (const auto& definition : content.visual_definitions.definitions()) {
        visual_models.insert(definition.model_asset);
        const auto* record = store.value().manifest().find_active(definition.model_asset);
        assert(record != nullptr);
        assert(record->kind == assets::AssetKind::model);
        auto payload = store.value().load_payload(*record);
        assert(payload);
        auto model = assets::decode_model_asset(payload.value().bytes);
        assert(model);
        visual_primitive_counts.emplace(definition.entity_prototype.value(),
                                        model.value().primitives.size());
        if (definition.id == *fallback) {
            fallback_primitive_count = model.value().primitives.size();
        }
        if (definition.entity_prototype == *player) {
            const auto capabilities = assets::model_capabilities(model.value());
            assert(capabilities.has_animation_clips);
            assert(capabilities.has_animated_nodes);
            assert(!capabilities.has_skins);
        }
        if (definition.model_asset == "base:models/props/foundation_material_showcase.gltf") {
            assert(model.value().primitives.size() == 3);
            assert(model.value().materials.size() == 3);
            assert(model.value().images.size() == 3);
            assert(model.value().primitives[0].material == 0);
            assert(model.value().primitives[1].material == 1);
            assert(model.value().primitives[2].material == 2);
            assert(model.value().materials[0].alpha_mode == assets::ModelAlphaMode::opaque);
            assert(model.value().materials[1].alpha_mode == assets::ModelAlphaMode::opaque);
            assert(model.value().materials[2].alpha_mode == assets::ModelAlphaMode::mask);
            assert(model.value().materials[2].alpha_cutoff == 0.5F);
            assert(model.value().materials[2].double_sided);
        }
        for (const auto& [role, clip_name] : definition.animation_clips) {
            (void)role;
            assert(assets::resolve_model_animation_clip(model.value(), clip_name));
        }
        for (const auto& [role, sound_event] : definition.sound_events) {
            (void)role;
            assert(content.sound_events.find(sound_event) != nullptr);
        }
    }
    assert(store.value().manifest().active_count() >= visual_models.size());
    for (const auto& event : content.sound_events.definitions()) {
        const auto* asset = content.asset_catalog.find_active(event.asset_id);
        assert(asset != nullptr);
        assert(asset->kind == assets::AssetKind::sound || asset->kind == assets::AssetKind::music);
        assert(std::filesystem::is_regular_file(asset->source_path));
        const auto* cooked = store.value().manifest().find_active(event.asset_id);
        assert(cooked != nullptr && cooked->kind == asset->kind);
        auto payload = store.value().load_payload(*cooked);
        assert(payload && payload.value().profile == "production");
        assert(payload.value().metadata.contains("audio.runtime_format"));
    }
    assert(fallback_primitive_count > 0);

    entities::VisualDefinitionRegistry shared_visuals;
    const auto* fallback_definition = content.visual_definitions.find(*fallback);
    assert(fallback_definition != nullptr);
    assert(shared_visuals.add(*fallback_definition));
    auto cache_probe = *fallback_definition;
    cache_probe.id = *core::PrototypeId::parse("base:visuals/cache_probe");
    cache_probe.entity_prototype = *core::PrototypeId::parse("base:entities/cache_probe");
    assert(shared_visuals.add(std::move(cache_probe)));

    renderer::Renderer cache_renderer;
    initialize_renderer(cache_renderer);
    game::ModelPresentationSystem shared_models;
    assert(shared_models.initialize(cache_renderer, shared_visuals,
                                    std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR}));
    assert(shared_models.stats().definition_count == 2);
    assert(shared_models.stats().loaded_model_count == 1);
    renderer::RenderCamera cache_camera;
    assert(cache_camera.set_aspect_ratio(640.0F / 360.0F));
    assert(cache_renderer.render(cache_camera));
    assert(cache_renderer.stats().resident_static_meshes == 1 + fallback_primitive_count);
    assert(shared_models.shutdown(cache_renderer));
    assert(cache_renderer.shutdown());

    renderer::Renderer renderer;
    initialize_renderer(renderer);
    game::ModelPresentationSystem models;
    assert(models.initialize(renderer, content.visual_definitions,
                             std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR}));
    assert(models.stats().definition_count == 4);
    assert(models.stats().loaded_model_count == 4);

    game::RenderSnapshot snapshot;
    snapshot.simulation_tick = 10;
    snapshot.objects.push_back(make_object(1, "base:entities/player", -0.75));
    snapshot.objects.push_back(make_object(2, "base:entities/test_animal", 0.75));
    snapshot.objects.push_back(make_object(4, "base:entities/foundation_material_showcase", 0.0));
    auto synchronized = models.synchronize(renderer, snapshot);
    assert(synchronized);
    assert(synchronized.value().models.inserted_entities == 3);
    assert(synchronized.value().models.evaluated_poses == 1);
    assert(synchronized.value().models.uploaded_palettes == 0);

    renderer::RenderCamera camera;
    assert(camera.set_aspect_ratio(640.0F / 360.0F));
    assert(renderer.render(camera));
    const auto presented_primitive_count =
        visual_primitive_counts.at("base:entities/player") +
        visual_primitive_counts.at("base:entities/test_animal") +
        visual_primitive_counts.at("base:entities/foundation_material_showcase");
    assert(renderer.stats().retained_objects == presented_primitive_count);
    assert(renderer.stats().retained_skin_palettes == 0);

    std::vector<std::string> warnings;
    core::set_log_sink([&](core::LogLevel level, std::string_view message) {
        if (level == core::LogLevel::warning) {
            warnings.emplace_back(message);
        }
    });
    snapshot.objects.push_back(make_object(3, "missing:entities/ghost", 1.5));
    synchronized = models.synchronize(renderer, snapshot);
    assert(synchronized);
    assert(synchronized.value().fallback_entity_count == 1);
    assert(synchronized.value().unresolved_visual_count == 1);
    assert(renderer.render(camera));
    assert(renderer.stats().retained_objects ==
           presented_primitive_count + fallback_primitive_count);
    synchronized = models.synchronize(renderer, snapshot);
    core::reset_log_sink();
    assert(synchronized);
    assert(warnings.size() == 1);
    assert(warnings.front().find("missing:entities/ghost") != std::string::npos);

    snapshot.objects.clear();
    synchronized = models.synchronize(renderer, snapshot);
    assert(synchronized);
    assert(synchronized.value().models.removed_entities == 4);
    assert(models.shutdown(renderer));
    assert(renderer.shutdown());

    entities::VisualDefinitionRegistry fallback_probes;
    assert(fallback_probes.add(*fallback_definition));
    auto missing_model = *content.visual_definitions.find_for_entity(*animal);
    missing_model.id = *core::PrototypeId::parse("test:visuals/missing_model");
    missing_model.entity_prototype = *core::PrototypeId::parse("test:entities/missing_model");
    missing_model.model_asset = "test:models/missing.gltf";
    assert(fallback_probes.add(std::move(missing_model)));
    auto missing_animation = *content.visual_definitions.find_for_entity(*player);
    missing_animation.id = *core::PrototypeId::parse("test:visuals/missing_animation");
    missing_animation.entity_prototype =
        *core::PrototypeId::parse("test:entities/missing_animation");
    missing_animation.animation_clips["walk"] = "MissingWalk";
    assert(fallback_probes.add(std::move(missing_animation)));

    renderer::Renderer fallback_renderer;
    initialize_renderer(fallback_renderer);
    game::ModelPresentationSystem fallback_models;
    warnings.clear();
    core::set_log_sink([&](core::LogLevel level, std::string_view message) {
        if (level == core::LogLevel::warning) {
            warnings.emplace_back(message);
        }
    });
    assert(fallback_models.initialize(fallback_renderer, fallback_probes,
                                      std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR}));
    core::reset_log_sink();
    assert(fallback_models.stats().definition_count == 3);
    assert(fallback_models.stats().loaded_model_count == 2);
    assert(fallback_models.stats().fallback_model_definition_count == 1);
    assert(fallback_models.stats().fallback_animation_mapping_count == 1);
    assert(fallback_models.stats().load_diagnostic_count == 2);
    assert(warnings.size() == 2);
    const auto diagnostics = fallback_models.load_diagnostics();
    assert(diagnostics.size() == 2);
    assert(diagnostics[0].logical_id == "test:models/missing.gltf");
    assert(diagnostics[0].source_path == "<missing>");
    assert(diagnostics[0].cooked_path == "<missing>");
    assert(diagnostics[0].failing_dependency == "test:models/missing.gltf");
    assert(diagnostics[0].fallback_used == fallback_definition->model_asset);
    assert(diagnostics[1].logical_id == "base:models/entities/test_player.glb");
    assert(diagnostics[1].source_path != "<missing>");
    assert(diagnostics[1].cooked_path != "<missing>");
    assert(diagnostics[1].failing_dependency == "test:visuals/missing_animation#animations/walk");
    assert(diagnostics[1].fallback_used == "idle=idle");

    game::RenderSnapshot fallback_snapshot;
    fallback_snapshot.simulation_tick = 20;
    fallback_snapshot.objects.push_back(make_object(10, "test:entities/missing_model", -0.5));
    auto animated_fallback = make_object(11, "test:entities/missing_animation", 0.5);
    animated_fallback.current_locomotion.kind = animation::LocomotionAnimationKind::walk;
    fallback_snapshot.objects.push_back(std::move(animated_fallback));
    auto fallback_synchronized = fallback_models.synchronize(fallback_renderer, fallback_snapshot);
    assert(fallback_synchronized);
    assert(fallback_synchronized.value().models.inserted_entities == 2);
    assert(fallback_synchronized.value().models.evaluated_poses == 1);
    assert(fallback_models.shutdown(fallback_renderer));
    assert(fallback_renderer.shutdown());
    return 0;
}
