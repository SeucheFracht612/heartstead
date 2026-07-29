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
    assert(content.visual_definitions.size() == 3);
    const auto player = core::PrototypeId::parse("base:entities/player");
    const auto animal = core::PrototypeId::parse("base:entities/test_animal");
    const auto fallback = core::PrototypeId::parse("base:visuals/fallback");
    assert(player && animal && fallback);
    assert(content.visual_definitions.find_for_entity(*player) != nullptr);
    assert(content.visual_definitions.find_for_entity(*animal) != nullptr);
    assert(content.visual_definitions.find(*fallback) != nullptr);

    auto store =
        assets::CookedAssetStore::load(std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR});
    assert(store);
    assert(store.value().manifest().validate_dependencies());
    std::unordered_set<std::string> visual_models;
    for (const auto& definition : content.visual_definitions.definitions()) {
        visual_models.insert(definition.model_asset);
        const auto* record = store.value().manifest().find_active(definition.model_asset);
        assert(record != nullptr);
        assert(record->kind == assets::AssetKind::model);
        auto payload = store.value().load_payload(*record);
        assert(payload);
        auto model = assets::decode_model_asset(payload.value().bytes);
        assert(model);
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

    renderer::Renderer renderer;
    initialize_renderer(renderer);
    game::ModelPresentationSystem models;
    assert(models.initialize(renderer, content.visual_definitions,
                             std::filesystem::path{HEARTSTEAD_TEST_COOKED_ASSET_DIR}));
    assert(models.stats().definition_count == 3);
    assert(models.stats().loaded_model_count == 3);

    game::RenderSnapshot snapshot;
    snapshot.simulation_tick = 10;
    snapshot.objects.push_back(make_object(1, "base:entities/player", -0.75));
    snapshot.objects.push_back(make_object(2, "base:entities/test_animal", 0.75));
    auto synchronized = models.synchronize(renderer, snapshot);
    assert(synchronized);
    assert(synchronized.value().models.inserted_entities == 2);
    assert(synchronized.value().models.evaluated_poses == 1);
    assert(synchronized.value().models.uploaded_palettes == 1);

    renderer::RenderCamera camera;
    assert(camera.set_aspect_ratio(640.0F / 360.0F));
    assert(renderer.render(camera));
    assert(renderer.stats().retained_objects == 2);
    assert(renderer.stats().retained_skin_palettes == 1);

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
    assert(renderer.stats().retained_objects == 3);
    synchronized = models.synchronize(renderer, snapshot);
    core::reset_log_sink();
    assert(synchronized);
    assert(warnings.size() == 1);
    assert(warnings.front().find("missing:entities/ghost") != std::string::npos);

    snapshot.objects.clear();
    synchronized = models.synchronize(renderer, snapshot);
    assert(synchronized);
    assert(synchronized.value().models.removed_entities == 3);
    assert(models.shutdown(renderer));
    assert(renderer.shutdown());
    return 0;
}
