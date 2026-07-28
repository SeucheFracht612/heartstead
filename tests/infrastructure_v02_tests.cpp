#include "engine/assets/asset_catalog.hpp"
#include "engine/assets/resource_pack.hpp"
#include "engine/debug/debug_overlay.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/net/replication.hpp"
#include "engine/net/server_command.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/networks/spatial_network.hpp"
#include "engine/renderer/world_render_list.hpp"
#include "engine/server/admin_service.hpp"
#include "engine/server_logs/server_log.hpp"
#include "engine/workpieces/material_crafting.hpp"
#include "engine/world/replication_delta.hpp"
#include "engine/world/world_snapshot.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"

#include <cassert>
#include <string>

namespace {

[[nodiscard]] heartstead::core::PrototypeId id(std::string value) {
    auto parsed = heartstead::core::PrototypeId::parse(value);
    assert(parsed);
    return *parsed;
}

void test_controlled_pack_policy() {
    using namespace heartstead;
    assets::ResourcePackManifest pack;
    pack.id = "visuals";
    pack.name = "Visuals";
    pack.version = "1";
    pack.target_namespace = "visuals";
    pack.shader_extensions = {assets::ShaderExtensionPoint::water};
    assert(assets::ResourcePackPolicy::validate_manifest(pack));
    assets::AssetRecord raw;
    raw.logical_id = "visuals:shaders/raw.spv";
    raw.kind = assets::AssetKind::shader;
    raw.source_kind = assets::AssetSourceKind::resource_pack;
    raw.source_id = "visuals";
    auto path = assets::VirtualPath::parse("visuals:shaders/raw.spv");
    assert(path);
    raw.virtual_path = path.value();
    raw.source_path = "raw.spv";
    auto status = assets::ResourcePackPolicy::validate_override(pack, raw);
    assert(!status);
    assert(status.error().code == "resource_pack.raw_vulkan_forbidden");

    auto allowed = raw;
    allowed.logical_id = "visuals:shaders/extensions/water/waves.slang";
    path = assets::VirtualPath::parse(allowed.logical_id);
    assert(path);
    allowed.virtual_path = path.value();
    allowed.source_path = "waves.slang";
    assert(assets::ResourcePackPolicy::validate_override(pack, allowed));

    auto undeclared = allowed;
    undeclared.logical_id = "visuals:shaders/extensions/foliage/leaves.slang";
    path = assets::VirtualPath::parse(undeclared.logical_id);
    assert(path);
    undeclared.virtual_path = path.value();
    status = assets::ResourcePackPolicy::validate_override(pack, undeclared);
    assert(!status);
    assert(status.error().code == "resource_pack.undeclared_shader_extension");

    auto unscoped = allowed;
    unscoped.logical_id = "visuals:shaders/waves.slang";
    path = assets::VirtualPath::parse(unscoped.logical_id);
    assert(path);
    unscoped.virtual_path = path.value();
    status = assets::ResourcePackPolicy::validate_override(pack, unscoped);
    assert(!status);
    assert(status.error().code == "resource_pack.unscoped_shader_forbidden");

    auto invalid_extension_pack = pack;
    invalid_extension_pack.shader_extensions = {static_cast<assets::ShaderExtensionPoint>(255)};
    status = assets::ResourcePackPolicy::validate_manifest(invalid_extension_pack);
    assert(!status);
    assert(status.error().code == "resource_pack.unknown_shader_extension");

    pack.gameplay_content = true;
    assert(!assets::ResourcePackPolicy::validate_manifest(pack));
}

void test_debug_overlay_registry() {
    using namespace heartstead;
    debug::DebugOverlayRegistry overlays;
    overlays.set_enabled(debug::DebugOverlayKind::chunk_boundaries, true);
    debug::DebugOverlayPrimitive primitive;
    primitive.overlay = debug::DebugOverlayKind::chunk_boundaries;
    primitive.position.anchor = {std::int64_t{1} << 55, -42, 9};
    primitive.label = "chunk 1";
    assert(overlays.submit(primitive));
    assert(overlays.frame_primitives().size() == 1);
    assert(overlays.primitives(debug::DebugOverlayKind::chunk_boundaries).size() == 1);
    overlays.clear_frame();
    assert(overlays.frame_primitives().empty());
}

void test_route_effects() {
    using namespace heartstead;
    networks::SpatialNetwork road(networks::NetworkKind::road);
    assert(road.add_node({networks::NetworkNodeId::from_value(1), {0, 0, 0}, 12, "a"}));
    assert(road.add_node({networks::NetworkNodeId::from_value(2), {8, 0, 0}, 8, "b"}));
    assert(road.add_edge({networks::NetworkEdgeId::from_value(1),
                          networks::NetworkNodeId::from_value(1),
                          networks::NetworkNodeId::from_value(2), 800, 6, false}));
    auto effects = road.route_effects(networks::NetworkNodeId::from_value(1),
                                      networks::NetworkNodeId::from_value(2));
    assert(effects.reachable && effects.bottleneck_capacity == 6);
    assert(effects.cart_speed_per_mille > 1000);
    assert(effects.pathfinding_reliability_per_mille == 800);
}

void test_clay_and_casting_foundations() {
    using namespace heartstead;
    workpieces::FiringOutcomeTable table{{{300, workpieces::FiringOutcome::fired},
                                          {600, workpieces::FiringOutcome::crazed},
                                          {850, workpieces::FiringOutcome::cracked},
                                          {1000, workpieces::FiringOutcome::shattered}}};
    assert(table.validate());
    assert(table.resolve(200, 7));
    workpieces::MoltenItemRecord molten;
    molten.item_id = core::SaveId::from_value(1);
    molten.material_id = id("base:materials/copper");
    molten.material_units = 100;
    molten.state = workpieces::MoltenState::molten;
    workpieces::MouldRecord mould;
    mould.mould_id = core::SaveId::from_value(2);
    mould.pattern_id = id("base:patterns/axe_head");
    const auto player = core::NetId::from_value(3);
    assert(workpieces::CastingRuntime::draw_for_pour(molten, player, 10, 20));
    assert(workpieces::CastingRuntime::pour(molten, mould, player, 15, 50));
    assert(mould.contained_material_units == 100 && mould.uses_remaining == 0);
    auto recycled = workpieces::CastingRuntime::recycle_units(100, workpieces::ToolWearBand::worn);
    assert(recycled && recycled.value() == 75);
}

void test_admin_ban_validation() {
    using namespace heartstead;
    auto uuid = player_profiles::PlayerUuid::parse("123e4567-e89b-12d3-a456-426614174000");
    assert(uuid);
    server::ServerBanRecord record{uuid, {}, "griefing", "2026-07-10T00:00:00Z", "console"};
    assert(record.validate());
    record.reason = "bad\nline";
    assert(!record.validate());
    record.reason = "griefing";
    record.created_at_utc = "2026-07-10|forged";
    assert(!record.validate());
    record.created_at_utc = "2026-07-10T00:00:00Z";
    record.address_hash.assign(257, 'a');
    assert(!record.validate());
}

void test_rotated_log_archive_codec() {
    const std::string text = "chat line 1\nchat line 2 aaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n";
    auto encoded = heartstead::server_logs::ServerLogArchiveCodec::encode(text);
    auto decoded = heartstead::server_logs::ServerLogArchiveCodec::decode(encoded);
    assert(decoded && decoded.value() == text);
    encoded.pop_back();
    assert(!heartstead::server_logs::ServerLogArchiveCodec::decode(encoded));
}

void test_camera_relative_draw_anchor() {
    using namespace heartstead;
    renderer::rhi::RenderMeshBinding draw;
    draw.vertex_buffer = {1};
    draw.vertex_count = 3;
    draw.material_id = id("base:materials/test");
    world::WorldPosition position;
    position.anchor = {std::int64_t{1} << 55, -4, 7};
    position.local_offset = {0.25, 0.5, 0.75};
    auto anchored =
        renderer::anchored_mesh_draw(draw, position, {{(std::int64_t{1} << 55) - 2, -4, 7}});
    assert(anchored);
    assert(anchored.value().world_anchor_x == (std::int64_t{1} << 55));
    assert(anchored.value().camera_relative_x == 2.25F);
}

void test_cubic_worldgen_caves_and_external_features() {
    using namespace heartstead;
    world::VoxelPalette palette;
    world::VoxelDefinition stone;
    stone.type = 1;
    stone.prototype_id = id("test:voxels/stone");
    stone.display_name = "Stone";
    stone.terrain_material = "stone";
    stone.mining_tool = "pickaxe";
    assert(palette.add(stone));
    world::RegionDescriptor region;
    region.id = "test_region";
    region.age = "age_0";
    region.biome_cluster = "temperate";
    region.resource_rules.push_back({stone.prototype_id, "terrain", 1.0});
    region.resource_rules.push_back({id("test:features/giant_tree"), "large_static_object", 1.0});
    world::RegionGraph graph;
    assert(graph.add_region(region));
    world::TerrainGenerationConfig config;
    config.world_seed = 9;
    config.region_id = "test_region";
    config.base_surface_y = 31;
    config.enable_caves = true;
    config.cave_frequency_per_mille = 1000;
    config.cave_min_depth = 8;
    config.feature_frequency_per_mille = 1000;
    auto underground = world::DeterministicTerrainGenerator::generate_chunk_with_features(
        {0, 0, 0}, config, graph, palette);
    assert(underground);
    assert(underground.value().features.empty());
    auto deep_cell = underground.value().chunk.get({0, 0, 0});
    assert(deep_cell && deep_cell.value().type == world::VoxelPalette::air_type);
    auto surface = world::DeterministicTerrainGenerator::generate_chunk_with_features(
        {0, 1, 0}, config, graph, palette);
    assert(surface && !surface.value().features.empty());
    assert(surface.value().features.front().kind ==
           world::GeneratedWorldFeatureKind::large_static_object);
}

void test_physical_resource_save_and_replication_shape() {
    using namespace heartstead;
    entities::PhysicalResourceRecord resource;
    resource.resource_id = core::SaveId::from_value(80);
    resource.prototype_id = id("test:entities/felled_tree");
    resource.cargo_prototype_id = id("test:cargo/log");
    resource.position.anchor = {std::int64_t{1} << 54, -3, 8};
    resource.kind = entities::PhysicalResourceKind::felled_tree;
    resource.state = entities::PhysicalResourceState::dynamic;
    resource.physics_body_id = physics::PhysicsBodyId::from_value(9);
    resource.mass_grams = 200'000;
    resource.volume_milliliters = 500'000;
    resource.allowed_transport_modes =
        cargo::CargoTransportModes::of({cargo::CargoTransportMode::cart});
    resource.segments.push_back({});
    assert(resource.validate());

    world::WorldStateDesc desc;
    desc.metadata.schema_version = 1;
    desc.metadata.game_version = "test";
    world::WorldState state_with_metadata(desc);
    assert(state_with_metadata.physical_resources().insert(resource));
    auto snapshot = world::WorldSnapshotBridge::export_snapshot(state_with_metadata);
    assert(snapshot && snapshot.value().entities.size() == 1);
    assert(snapshot.value().entities.front().encoded_state.starts_with(
        entities::physical_resource_state_magic));
    auto loaded = world::WorldSnapshotBridge::import_snapshot(snapshot.value());
    assert(loaded);
    const auto* restored = loaded.value().physical_resources().find(resource.resource_id);
    assert(restored != nullptr && restored->needs_physics_rebuild);
    assert(!restored->physics_body_id.is_valid());
    assert(restored->position.anchor == resource.position.anchor);

    net::ReplicationBatch batch;
    batch.command_sequence = 4;
    batch.events.push_back({"physical_resource.settled", resource.resource_id, "settled"});
    auto delta = world::materialize_replication_delta(state_with_metadata, batch);
    assert(delta.entities.size() == 1);
    world::WorldState client;
    auto applied = world::apply_replication_delta(client, delta);
    assert(applied && client.physical_resources().find(resource.resource_id) != nullptr);
}

void test_failed_commands_restore_world_and_external_ids() {
    using namespace heartstead;
    world::WorldState state;
    save::SaveIdAllocator ids(50);
    net::ServerCommandDispatcher dispatcher;
    assert(dispatcher.register_command(
        {"test.fail_transaction", true, true,
         [](const net::CommandEnvelope&, const net::CommandExecutionContext& context,
            world::WorldOperation& operation) {
             assert(context.world_state != nullptr && context.save_ids != nullptr);
             assert(context.world_state->advance_world_time(10));
             assert(operation.reserve_save_id(*context.save_ids));
             assert(operation.record_mutation("staged mutation"));
             return core::Status::failure("test.failure", "intentional failure");
         }}));
    net::CommandExecutionContext context;
    context.world_state = &state;
    context.save_ids = &ids;
    auto result = dispatcher.dispatch(
        {1, core::NetId::from_value(1), "test.fail_transaction", {}, 0}, context);
    assert(!result && state.world_time() == 0);
    assert(ids.peek_next() == core::SaveId::from_value(50));
}

void test_fragment_reassembly_has_aggregate_budget() {
    using namespace heartstead::net;
    TransportPacketFragmentCodecConfig config;
    config.max_packet_bytes = 16;
    config.max_pending_packets = 1;
    config.max_pending_packet_bytes = 16;
    TransportPacketReassembler reassembler(config);
    TransportPacketFragment first{1, 0, 2, 16, "12345678"};
    TransportPacketFragment second{2, 0, 2, 16, "abcdefgh"};
    assert(reassembler.accept_fragment(first));
    auto rejected = reassembler.accept_fragment(second);
    assert(!rejected && rejected.error().code == "transport_fragment.pending_budget_exceeded");
    auto mismatched = first;
    mismatched.total_packet_bytes = 15;
    auto mismatch_result = reassembler.accept_fragment(mismatched);
    assert(!mismatch_result &&
           mismatch_result.error().code == "transport_fragment.mismatched_packet_metadata");
    assert(reassembler.accept_fragment(second));
}

void test_asset_namespaces_do_not_collide() {
    using namespace heartstead;
    assets::AssetCatalog catalog;
    const auto add = [&catalog](std::string logical, std::string name_space) {
        auto path = assets::VirtualPath::parse(logical);
        assert(path);
        assets::AssetRecord record;
        record.logical_id = logical;
        record.virtual_path = path.value();
        record.source_id = std::move(name_space);
        record.kind = assets::AssetKind::texture;
        return catalog.add(std::move(record));
    };
    assert(add("base:textures/shared.png", "base"));
    assert(add("other:textures/shared.png", "other"));
    assert(catalog.active_count() == 2);
    assert(catalog.find_active("base:textures/shared.png") != nullptr);
    assert(catalog.find_active("other:textures/shared.png") != nullptr);
}

void test_replication_codec_rejects_trailing_data() {
    using namespace heartstead;
    net::ReplicationBatch batch;
    batch.command_sequence = 7;
    batch.replication_sequence = 9;
    batch.source_client_id = core::NetId::from_value(3);
    batch.command_type = "world.set_voxel";
    batch.events.push_back(
        world::OperationEvent{"world.voxel_changed", core::SaveId::from_value(4), "changed"});
    const auto encoded = net::ReplicationTextCodec::encode(batch);
    assert(net::ReplicationTextCodec::decode(encoded));
    const auto binary = net::ReplicationBinaryCodec::encode(batch);
    assert(binary.size() < encoded.size());
    auto decoded_binary = net::ReplicationBinaryCodec::decode(binary);
    assert(decoded_binary);
    assert(decoded_binary.value().replication_sequence == 9);
    assert(decoded_binary.value().command_sequence == 7);
    assert(decoded_binary.value().events.size() == 1);
    assert(decoded_binary.value().events.front().type == "world.voxel_changed");
    assert(decoded_binary.value().events.front().subject == core::SaveId::from_value(4));
    assert(decoded_binary.value().events.front().message == "changed");
    assert(!net::ReplicationBinaryCodec::decode(binary.substr(0, binary.size() - 1)));
    auto trailing = net::ReplicationTextCodec::decode(encoded + "sequence=10\n");
    assert(!trailing && trailing.error().code == "replication.trailing_data");
}

} // namespace

int main() {
    test_controlled_pack_policy();
    test_debug_overlay_registry();
    test_route_effects();
    test_clay_and_casting_foundations();
    test_admin_ban_validation();
    test_rotated_log_archive_codec();
    test_camera_relative_draw_anchor();
    test_cubic_worldgen_caves_and_external_features();
    test_physical_resource_save_and_replication_shape();
    test_failed_commands_restore_world_and_external_ids();
    test_fragment_reassembly_has_aggregate_budget();
    test_asset_namespaces_do_not_collide();
    test_replication_codec_rejects_trailing_data();
}
