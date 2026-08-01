#include "engine/content/content_validation.hpp"
#include "engine/entities/physical_resource.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/fluids/fluid_state.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/game_inspection.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <limits>
#include <string>
#include <thread>
#include <vector>

using namespace heartstead;

namespace {

struct TestFeatureComponent {
    std::uint32_t value = 0;
};

class ITestFeatureService {
  public:
    virtual ~ITestFeatureService() = default;
    [[nodiscard]] virtual std::uint32_t value() const noexcept = 0;
};

class TestFeatureService final : public ITestFeatureService {
  public:
    [[nodiscard]] std::uint32_t value() const noexcept override {
        return 42;
    }
};

class TestGameplayModule final : public game::IGameplayModule {
  public:
    [[nodiscard]] std::string_view module_id() const noexcept override {
        return "test.feature";
    }

    [[nodiscard]] core::Status register_components(game::ComponentRegistry& registry) override {
        return registry.register_component<TestFeatureComponent>("test.feature_component");
    }

    [[nodiscard]] core::Status register_services(game::DomainServiceRegistry& registry) override {
        return registry.register_service<ITestFeatureService>(
            "test.feature_service", std::make_shared<TestFeatureService>());
    }

    [[nodiscard]] core::Status
    register_commands(game::GameplayRegistrationContext& context) override {
        return context.commands.register_command({
            "test.feature.set",
            true,
            true,
            [](const net::CommandEnvelope&, const net::CommandExecutionContext& command_context,
               world::WorldOperation& operation) {
                if (command_context.world_state == nullptr) {
                    return core::Status::failure("test_feature.world_missing",
                                                 "test feature requires authoritative world state");
                }
                auto status = command_context.world_state->mod_states().insert(
                    {"test", "feature_visible", "true"});
                if (!status) {
                    return status;
                }
                status = operation.record_mutation("set test feature visibility");
                if (!status) {
                    return status;
                }
                operation.mark_save_dirty();
                operation.mark_replication_dirty();
                operation.emit_event({"test.feature.delta", {}, "visible"});
                return core::Status::ok();
            },
        });
    }

    [[nodiscard]] core::Status
    register_systems(game::GameplayRegistrationContext& context) override {
        return context.scheduler.register_system({
            "test.feature.update",
            simulation::SimulationPhase::gameplay,
            {"runtime.physics"},
            [this](simulation::SimulationContext&) {
                ++update_count;
                return core::Status::ok();
            },
        });
    }

    [[nodiscard]] core::Status
    register_serializers(game::SerializationRegistry& registry) override {
        return registry.register_schema({"test.feature.state", 1});
    }

    [[nodiscard]] core::Status register_persistence(game::PersistenceRegistry& registry) override {
        return registry.register_persistence(
            {"test.feature.state", 1,
             [this](const world::WorldState& world, save::SaveSnapshot& snapshot) {
                 ++persistence_capture_count;
                 if (world.mod_states().find("test", "feature_visible") == nullptr ||
                     std::ranges::none_of(snapshot.mod_states, [](const auto& record) {
                         return record.mod_id == "test" && record.state_key == "feature_visible" &&
                                record.encoded_state == "true";
                     })) {
                     return core::Status::failure(
                         "test_feature.capture_missing_state",
                         "test feature persistence capture did not receive authoritative state");
                 }
                 return core::Status::ok();
             },
             [this](const save::SaveSnapshot& snapshot, world::WorldState& world) {
                 ++persistence_restore_count;
                 const auto saved =
                     std::ranges::find_if(snapshot.mod_states, [](const auto& record) {
                         return record.mod_id == "test" && record.state_key == "feature_visible" &&
                                record.encoded_state == "true";
                     });
                 if (saved == snapshot.mod_states.end() ||
                     world.mod_states().find("test", "feature_visible") == nullptr) {
                     return core::Status::failure(
                         "test_feature.restore_missing_state",
                         "test feature persistence restore did not receive imported state");
                 }
                 restored_from_snapshot = true;
                 client_visible = true;
                 client_revision = 1;
                 return core::Status::ok();
             }});
    }

    [[nodiscard]] core::Status register_replication(game::ReplicationRegistry& registry) override {
        return registry.register_replication(
            {"test.feature.delta", 1, true, false,
             [this](const world::OperationEvent& event, game::ClientRuntime&) {
                 if (event.message != "visible") {
                     return core::Status::failure("test_feature.invalid_delta",
                                                  "test feature delta is invalid");
                 }
                 client_visible = true;
                 ++client_revision;
                 return core::Status::ok();
             }});
    }

    [[nodiscard]] core::Status
    register_presentation(game::PresentationRegistry& registry) override {
        return registry.register_adapter(
            {"test.feature.presentation", 1,
             [this](const game::ClientRuntime&, game::PresentationWorld& presentation) {
                 game::PresentationAdapterStats stats;
                 if (!client_visible) {
                     return core::Result<game::PresentationAdapterStats>::success(stats);
                 }
                 const auto source = core::NetId::from_value(9'001);
                 const auto* previous = presentation.find_object(source);
                 game::PresentationObjectUpdate update;
                 update.source_net_id = source;
                 update.visual_prototype = *core::PrototypeId::parse("test:feature/marker");
                 update.transform.position = world::WorldPosition{12.0, 2.0, 12.0};
                 update.local_bounds = {{-0.25F, -0.25F, -0.25F}, {0.25F, 0.25F, 0.25F}};
                 update.source_revision = client_revision;
                 auto synchronized = presentation.upsert_object(update);
                 if (!synchronized) {
                     return core::Result<game::PresentationAdapterStats>::failure(
                         synchronized.error().code, synchronized.error().message);
                 }
                 if (previous == nullptr) {
                     ++stats.inserted_objects;
                 } else if (previous->source_revision == client_revision) {
                     ++stats.unchanged_objects;
                 } else {
                     ++stats.updated_objects;
                 }
                 return core::Result<game::PresentationAdapterStats>::success(stats);
             }});
    }

    std::uint32_t update_count = 0;
    std::uint32_t persistence_capture_count = 0;
    std::uint32_t persistence_restore_count = 0;
    std::uint64_t client_revision = 0;
    bool client_visible = false;
    bool restored_from_snapshot = false;
};

class FailingPresentationModule final : public game::IGameplayModule {
  public:
    [[nodiscard]] std::string_view module_id() const noexcept override {
        return "test.failing_presentation";
    }

    [[nodiscard]] core::Status
    register_presentation(game::PresentationRegistry& registry) override {
        return registry.register_adapter(
            {"test.failing_presentation.adapter", 1,
             [](const game::ClientRuntime&, game::PresentationWorld&) {
                 return core::Result<game::PresentationAdapterStats>::failure(
                     "test.presentation_failed", "intentional presentation failure");
             }});
    }
};

class FailingPersistenceModule final : public game::IGameplayModule {
  public:
    [[nodiscard]] std::string_view module_id() const noexcept override {
        return "test.failing_persistence";
    }

    [[nodiscard]] core::Status register_persistence(game::PersistenceRegistry& registry) override {
        return registry.register_persistence(
            {"test.failing_persistence.state", 1,
             [](const world::WorldState&, save::SaveSnapshot&) {
                 return core::Status::failure("test.persistence_failed",
                                              "intentional persistence failure");
             },
             [](const save::SaveSnapshot&, world::WorldState&) { return core::Status::ok(); }});
    }
};

class FailingReplicationModule final : public game::IGameplayModule {
  public:
    [[nodiscard]] std::string_view module_id() const noexcept override {
        return "test.failing_replication";
    }

    [[nodiscard]] core::Status
    register_commands(game::GameplayRegistrationContext& context) override {
        return context.commands.register_command(
            {"test.failing_replication.trigger", true, true,
             [](const net::CommandEnvelope&, const net::CommandExecutionContext&,
                world::WorldOperation& operation) {
                 auto status = operation.record_mutation("trigger failing replication");
                 if (!status) {
                     return status;
                 }
                 operation.mark_save_dirty();
                 operation.mark_replication_dirty();
                 operation.emit_event({"test.failing_replication.delta", {}, "invalid"});
                 return core::Status::ok();
             }});
    }

    [[nodiscard]] core::Status register_replication(game::ReplicationRegistry& registry) override {
        return registry.register_replication(
            {"test.failing_replication.delta", 1, true, false,
             [](const world::OperationEvent&, game::ClientRuntime&) {
                 return core::Status::failure("test.replication_failed",
                                              "intentional replication failure");
             }});
    }
};

std::filesystem::path source_root() {
    return std::filesystem::path(HEARTSTEAD_TEST_SOURCE_DIR);
}

game::GameRuntime make_runtime(const content::ContentValidationReport& content_report) {
    auto runtime = game::GameRuntime::initialize(game::GameRuntimeConfig{}, content_report);
    assert(runtime);
    return std::move(runtime).value();
}

game::SessionRequest make_session_request(const content::ContentValidationReport& report) {
    auto metadata = content::save_metadata_from_content_report(report, "runtime-spine-test",
                                                               game::foundation::world_seed);
    assert(metadata);
    game::SessionRequest request;
    request.metadata = std::move(metadata).value();
    request.scenario_id = "base:scenarios/homestead";
    return request;
}

std::string set_voxel_payload() {
    net::CommandPayload payload;
    assert(payload.set("chunk", "0|0|0"));
    assert(payload.set("voxel", "1|2|3"));
    assert(payload.set("prototype", "base:voxels/clay"));
    return net::CommandPayloadTextCodec::encode(payload);
}

void test_local_runtime_advances_authority_through_loopback() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);

    game::RuntimeConfiguration config;
    config.create_server = true;
    config.create_client = true;
    config.create_renderer = false;
    config.create_audio = false;
    config.use_in_memory_transport = true;
    config.headless = true;
    config.fixed_step = {60, 4, 250'000};
    assert(runtime.start_session(config, make_session_request(report)));
    assert(runtime.session() != nullptr);
    assert(runtime.session()->server() != nullptr);
    assert(runtime.session()->client() != nullptr);
    assert(runtime.session()->client()->is_connected());

    runtime.session()->server()->world().chunks().get_or_create({0, 0, 0}).clear_all_dirty();
    assert(runtime.submit_command("world.set_voxel", set_voxel_payload(), 10));

    auto frame = runtime.run_frame({16'667, 17});
    assert(frame);
    assert(frame.value().fixed_step.step_count == 1);
    assert(frame.value().server_ticks.size() == 1);
    assert(frame.value().server_ticks.front().commands.command_message_count == 1);
    assert(frame.value().server_ticks.front().commands.command_reports.size() == 1);
    assert(frame.value().server_ticks.front().commands.command_reports.front().success);
    assert(frame.value().client.command_result_count == 1);
    assert(frame.value().authoritative_world_tick == 0);
    assert(runtime.session()->server()->events().is_sealed());

    auto voxel = runtime.session()->server()->world().chunks().get({0, 0, 0}, {1, 2, 3});
    assert(voxel);
    const auto clay = report.voxel_palette.type_for(*core::PrototypeId::parse("base:voxels/clay"));
    assert(clay.has_value());
    assert(voxel.value().type == *clay);
    assert(runtime.session()->client()->command_results().size() == 1);
    assert(runtime.session()->client()->command_results().front().success);

    auto no_tick = runtime.run_frame({0, 17});
    assert(no_tick);
    assert(no_tick.value().fixed_step.step_count == 0);
    assert(no_tick.value().server_ticks.empty());
    assert(runtime.shutdown());
    assert(runtime.session() == nullptr);
}

void test_client_command_result_history_is_bounded() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    assert(runtime.start_session(config, make_session_request(report)));

    constexpr auto overflow_count = 32U;
    const auto command_count =
        static_cast<std::uint32_t>(game::client_command_result_history_capacity) + overflow_count;
    for (std::uint32_t index = 0; index < command_count; ++index) {
        assert(runtime.submit_command("missing.foundation_command", "", 10));
    }
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame);
    assert(frame.value().client.command_result_count == command_count);
    assert(frame.value().client.retained_command_result_count ==
           game::client_command_result_history_capacity);
    assert(frame.value().client.dropped_command_result_count == overflow_count);
    const auto history = runtime.session()->client()->command_results();
    assert(history.size() == game::client_command_result_history_capacity);
    assert(history.front().sequence == overflow_count + 1U);
    assert(history.back().sequence == command_count);
    assert(!history.back().success);
    assert(runtime.shutdown());
}

void test_selected_scenario_drives_authoritative_bootstrap() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    assert(runtime.start_session({}, make_session_request(report)));
    const auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    const auto& world = session->server()->world();
    const auto* scenario_id = world.mod_states().find("engine", "scenario.id");
    const auto* start_region = world.mod_states().find("engine", "scenario.start_region");
    const auto* spawn_mode = world.mod_states().find("engine", "scenario.spawn_mode");
    const auto* layout_version = world.mod_states().find(game::foundation::layout_state_mod,
                                                         game::foundation::layout_state_key);
    assert(scenario_id != nullptr && scenario_id->encoded_state == "base:scenarios/homestead");
    assert(start_region != nullptr && start_region->encoded_state == "temperate_valley");
    assert(spawn_mode != nullptr && spawn_mode->encoded_state == "homestead");
    assert(layout_version != nullptr &&
           layout_version->encoded_state == std::to_string(game::foundation::layout_version));
    assert(world.metadata().world_seed == game::foundation::world_seed);
    assert(world.cargo().count() == 1);
    const auto cargo = world.cargo().records();
    assert(cargo.front()->prototype_id == *core::PrototypeId::parse("base:cargo/heavy_log"));
    const auto showcase = core::PrototypeId::parse(
        "base:entities/foundation_material_showcase");
    assert(showcase);
    const auto scene_entities = world.entities().records();
    const auto scene_entity = std::ranges::find(
        scene_entities, showcase.value(),
        [](const entities::EntityRecord* record) { return record->prototype_id; });
    assert(scene_entity != scene_entities.end());
    assert((*scene_entity)->persistent);
    assert((*scene_entity)->transform.position == (world::WorldPosition{8.5, 1.0, 12.5}));

    const auto* player = session->server()->player_for_client(session->client()->client_id());
    assert(player != nullptr);
    assert(player->state.position == (world::WorldPosition{8.5, 1.0, 8.5}));
    const auto* inventory = world.inventories().find(player->save_id);
    assert(inventory != nullptr && inventory->stacks.size() == 2);
    const auto& private_access =
        session->server()->host().replication_relevance_policy().private_access_rules;
    assert(private_access.size() == 1);
    assert(private_access.front().client_id == session->client()->client_id());
    assert(private_access.front().private_subjects == std::vector{player->save_id});
    assert(inventory->stacks[0].prototype_id == *core::PrototypeId::parse("base:items/raw_clay"));
    assert(inventory->stacks[1].prototype_id == *core::PrototypeId::parse("base:items/nails"));
    assert(
        std::ranges::all_of(inventory->stacks, [](const auto& stack) { return stack.count == 1; }));
    assert(runtime.shutdown());
}

void test_session_rejects_unknown_or_wrong_kind_scenarios() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    auto missing = make_session_request(report);
    missing.scenario_id = "base:scenarios/missing";
    auto status = runtime.start_session({}, std::move(missing));
    assert(!status && status.error().code == "runtime_session.scenario_missing");

    auto wrong_kind = make_session_request(report);
    wrong_kind.scenario_id = "base:items/raw_clay";
    status = runtime.start_session({}, std::move(wrong_kind));
    assert(!status && status.error().code == "scenario_prototype.kind_mismatch");
}

void test_dedicated_headless_runtime_uses_same_scheduler() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.create_server = true;
    config.create_client = false;
    config.headless = true;
    assert(runtime.start_session(config, make_session_request(report)));
    assert(runtime.session()->server() != nullptr);
    assert(runtime.session()->client() == nullptr);

    auto frame = runtime.run_frame({50'000, 50});
    assert(frame);
    assert(frame.value().fixed_step.step_count == 3);
    assert(frame.value().server_ticks.size() == 3);
    assert(frame.value().authoritative_world_tick == 1);
    const auto no_tick = runtime.run_frame({0, 50});
    assert(no_tick);
    assert(no_tick.value().fixed_step.step_count == 0);
    assert(no_tick.value().authoritative_world_tick == 1);
    const auto names = runtime.session()->server()->scheduler().ordered_system_names();
    assert(names.front() == "runtime.command_gateway");
    assert(names[1] == "runtime.chunk_collision");
    assert(names[2] == "runtime.chunk_fluids");
    assert(names[3] == "runtime.character_movement");
    assert(names.back() == "runtime.replication");
    assert(runtime.shutdown());
}

void test_external_listen_runtime_uses_true_remote_endpoint() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.use_in_memory_transport = false;
    config.server_bind_endpoint = {"127.0.0.1", 0};
    assert(runtime.start_session(config, make_session_request(report)));
    assert(runtime.session()->server() != nullptr);
    assert(runtime.session()->client() != nullptr);
    assert(!runtime.session()->client()->is_connected());

    std::int64_t now_ms = 0;
    for (std::uint32_t frame_index = 0;
         frame_index < 8 && !runtime.session()->client()->is_connected(); ++frame_index) {
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
    }
    assert(runtime.session()->client()->is_connected());
    assert(runtime.session()->server()->host().connected_client_count() == 1);
    assert(runtime.session()->server()->player_for_client(
               runtime.session()->client()->client_id()) != nullptr);

    const auto predicted_start =
        runtime.session()->client()->local_player_snapshot()->state.position;
    movement::PlayerInputFrame movement_input;
    movement_input.tick = 1;
    movement_input.sequence = 1;
    movement_input.move_z = 32'767;
    assert(runtime.session()->submit_player_input(movement_input, now_ms));
    const auto predicted_position =
        runtime.session()->client()->local_player_snapshot()->state.position;
    assert(predicted_position.relative_to(predicted_start.anchor).z >
           predicted_start.local_offset.z);
    now_ms += 17;
    auto movement_frame = runtime.run_frame({16'667, now_ms});
    assert(movement_frame);
    assert(movement_frame.value().server_ticks.front().commands.control_message_count == 1);
    assert(movement_frame.value().server_ticks.front().accepted_movement_input_count == 1);
    assert(movement_frame.value().client.acknowledged_input_count == 1);

    assert(runtime.submit_command("world.set_voxel", set_voxel_payload(), now_ms));
    for (std::uint32_t frame_index = 0; frame_index < 4; ++frame_index) {
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
    }
    const auto* replicated = runtime.session()->client()->world().chunks().find({0, 0, 0});
    assert(replicated != nullptr);
    const auto cell = replicated->get({1, 2, 3});
    assert(cell && cell.value().type != 0);
    assert(runtime.shutdown());
}

void test_two_remote_clients_predict_and_interpolate() {
    if (!net::transport_backend_info(net::TransportBackend::external_library).available) {
        return;
    }
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto server = make_runtime(report);
    game::RuntimeConfiguration server_config;
    server_config.create_client = false;
    server_config.use_in_memory_transport = false;
    server_config.server_bind_endpoint = {"127.0.0.1", 0};
    assert(server.start_session(server_config, make_session_request(report)));
    const auto endpoint = server.session()->server()->host().local_endpoint();
    assert(endpoint.has_value() && endpoint->port != 0);

    auto first = make_runtime(report);
    auto second = make_runtime(report);
    game::RuntimeConfiguration client_config;
    client_config.create_server = false;
    client_config.create_client = true;
    client_config.use_in_memory_transport = false;
    client_config.remote_server_endpoint = endpoint;
    assert(first.start_session(client_config, make_session_request(report)));
    assert(second.start_session(client_config, make_session_request(report)));

    std::int64_t now_ms = 0;
    for (std::uint32_t frame_index = 0; frame_index < 12; ++frame_index) {
        now_ms += 17;
        assert(server.run_frame({16'667, now_ms}));
        assert(first.run_frame({16'667, now_ms}));
        assert(second.run_frame({16'667, now_ms}));
        if (first.session()->client()->is_connected() &&
            second.session()->client()->is_connected() &&
            first.session()->client()->local_player_snapshot() != nullptr &&
            second.session()->client()->local_player_snapshot() != nullptr) {
            break;
        }
    }
    assert(first.session()->client()->is_connected());
    assert(second.session()->client()->is_connected());
    const auto first_player = first.session()->client()->local_player_net_id();
    assert(first_player.is_valid() &&
           first_player != second.session()->client()->local_player_net_id());
    const auto start = first.session()->client()->local_player_snapshot()->state.position;
    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    assert(first.session()->submit_player_input(input, now_ms));
    const auto immediate = first.session()->client()->local_player_snapshot()->state.position;
    assert(immediate.relative_to(start.anchor).z > start.local_offset.z);

    game::RuntimeFrameStats second_frame;
    bool observed_remote_movement = false;
    for (std::uint32_t frame_index = 0; frame_index < 60; ++frame_index) {
        now_ms += 17;
        assert(server.run_frame({16'667, now_ms}));
        assert(first.run_frame({16'667, now_ms}));
        auto observed = second.run_frame({16'667, now_ms});
        assert(observed);
        second_frame = std::move(observed).value();
        const auto* candidate = second.session()->client()->player_snapshot(first_player);
        observed_remote_movement =
            candidate != nullptr &&
            candidate->state.position.relative_to(start.anchor).z > start.local_offset.z;
        if (observed_remote_movement) {
            break;
        }
    }
    const auto* interpolated = second.session()->client()->player_snapshot(first_player);
    assert(interpolated != nullptr);
    assert(observed_remote_movement);
    assert(second_frame.client.interpolated_player_count >= 1);

    constexpr world::BlockCoord remote_edit{9, 0, 8};
    assert(first.session()->submit_remove_voxel({remote_edit}, now_ms));
    bool first_observed_edit = false;
    bool second_observed_edit = false;
    for (std::uint32_t frame_index = 0; frame_index < 30; ++frame_index) {
        now_ms += 17;
        assert(server.run_frame({16'667, now_ms}));
        assert(first.run_frame({16'667, now_ms}));
        assert(second.run_frame({16'667, now_ms}));
        first_observed_edit =
            first_observed_edit || !first.session()->client()->accepted_voxel_edits().empty();
        second_observed_edit =
            second_observed_edit || !second.session()->client()->accepted_voxel_edits().empty();
        if (first_observed_edit && second_observed_edit) {
            break;
        }
    }
    assert(first_observed_edit && second_observed_edit);
    const auto edit_address = world::block_to_chunk_local(remote_edit);
    const auto first_cell =
        first.session()->client()->world().chunks().get(edit_address.chunk, edit_address.local);
    const auto second_cell =
        second.session()->client()->world().chunks().get(edit_address.chunk, edit_address.local);
    assert(first_cell && first_cell.value().is_air());
    assert(second_cell && second_cell.value().is_air());

    assert(server.shutdown());
    now_ms += 16'000;
    auto timed_out = first.run_frame({16'667, now_ms});
    assert(!timed_out && timed_out.error().code == "transport_client.idle_timeout");
    assert(first.shutdown());
    assert(second.shutdown());
}

void test_jolt_runtime_moves_on_cooked_terrain() {
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.physics_backend = physics::PhysicsBackend::jolt;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);

    std::int64_t now_ms = 0;
    for (std::uint32_t attempt = 0;
         attempt < 100 && session->server()->chunk_collision().find({0, 0, 0}) == nullptr;
         ++attempt) {
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(session->server()->chunk_collision().find({0, 0, 0}) != nullptr);
    const auto client_id = session->client()->client_id();
    const auto* before = session->server()->player_for_client(client_id);
    assert(before != nullptr);
    const auto start = before->state.position;

    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    assert(session->submit_player_input(input, now_ms));
    for (std::uint32_t frame_index = 0; frame_index < 60; ++frame_index) {
        now_ms += 17;
        auto frame = runtime.run_frame({16'667, now_ms});
        assert(frame);
    }
    const auto* after = session->server()->player_for_client(client_id);
    assert(after != nullptr);
    assert(after->state.position.relative_to(start.anchor).z > start.local_offset.z + 4.0);
    assert(std::abs(after->state.position.approximate_global().y - 1.0) < 0.06);

    auto* terrace_player = session->server()->player_for_client(client_id);
    assert(terrace_player != nullptr);
    terrace_player->state.position = {4.5, 1.0, 12.5};
    terrace_player->state.velocity = {};
    terrace_player->state.fall_origin = terrace_player->state.position;
    terrace_player->state.scripted_start = terrace_player->state.position;
    terrace_player->state.scripted_target = terrace_player->state.position;
    terrace_player->state.mode = movement::PlayerControllerMode::grounded;
    terrace_player->state.grounded = true;

    bool reached_terrace_top = false;
    bool descended_from_terrace = false;
    for (std::uint64_t sequence = 61; sequence <= 300; ++sequence) {
        input.tick = sequence;
        input.sequence = sequence;
        assert(session->submit_player_input(input, now_ms));
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        const auto* current = session->server()->player_for_client(client_id);
        assert(current != nullptr);
        const auto position = current->state.position.approximate_global();
        reached_terrace_top |= position.z >= 17.5 && position.z <= 21.0 && position.y >= 2.9;
        descended_from_terrace |=
            reached_terrace_top && position.z >= 24.0 && position.y <= 1.06 &&
            current->state.grounded;
        if (descended_from_terrace) {
            break;
        }
    }
    assert(reached_terrace_top);
    assert(descended_from_terrace);

    const auto water_id = core::PrototypeId::parse("base:voxels/water");
    assert(water_id.has_value());
    const auto* water_definition = report.voxel_palette.find_by_prototype(*water_id);
    assert(water_definition != nullptr);
    const auto pool_cell =
        session->server()->world().chunks().get(world::chunk_coord_for_block({25, 0, 15}),
                                               world::local_coord_for_block({25, 0, 15}));
    assert(pool_cell && pool_cell.value().type == water_definition->type);
    const auto pool_state = world::decode_fluid_state(pool_cell.value().state_bits);
    assert(pool_state && pool_state.value().source);

    auto* swimming_player = session->server()->player_for_client(client_id);
    assert(swimming_player != nullptr);
    swimming_player->state.position = {26.5, 1.0, 12.5};
    swimming_player->state.velocity = {};
    swimming_player->state.fall_origin = swimming_player->state.position;
    swimming_player->state.scripted_start = swimming_player->state.position;
    swimming_player->state.scripted_target = swimming_player->state.position;
    swimming_player->state.mode = movement::PlayerControllerMode::grounded;
    swimming_player->state.grounded = true;

    bool entered_pool = false;
    bool selected_swim_animation = false;
    bool exited_pool = false;
    const auto jump = movement::input_button_bit(movement::PlayerInputButton::jump);
    for (std::uint64_t sequence = 301; sequence <= 660; ++sequence) {
        input.tick = sequence;
        input.sequence = sequence;
        input.move_z = entered_pool ? -32'767 : 32'767;
        input.held_buttons = entered_pool ? jump : 0U;
        assert(session->submit_player_input(input, now_ms));
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        const auto* current = session->server()->player_for_client(client_id);
        assert(current != nullptr);
        entered_pool |= current->state.mode == movement::PlayerControllerMode::swimming;
        selected_swim_animation |=
            current->state.locomotion_animation.kind ==
            animation::LocomotionAnimationKind::swim;
        exited_pool =
            entered_pool && current->state.mode != movement::PlayerControllerMode::swimming &&
            current->state.position.approximate_global().z < 13.9;
        if (exited_pool) {
            break;
        }
    }
    assert(entered_pool);
    assert(selected_swim_animation);
    assert(exited_pool);
    assert(runtime.shutdown());
}

void test_collision_revision_preserves_pending_look_and_position() {
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.physics_backend = physics::PhysicsBackend::jolt;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);

    std::int64_t now_ms = 0;
    for (std::uint32_t attempt = 0;
         attempt < 100 && session->server()->chunk_collision().find({0, 0, 0}) == nullptr;
         ++attempt) {
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(session->server()->chunk_collision().find({0, 0, 0}) != nullptr);

    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.yaw_centidegrees = -3'500;
    assert(session->submit_player_input(input, now_ms));
    now_ms += 17;
    auto oriented = runtime.run_frame({16'667, now_ms});
    assert(oriented);
    const auto* before = session->client()->local_player_snapshot();
    assert(before != nullptr && before->state.yaw_centidegrees == input.yaw_centidegrees);
    const auto before_position = before->state.position;
    const auto before_collision_revision = before->collision_world_revision;

    constexpr world::BlockCoord edit{8, 0, 10};
    const auto cell_before = session->server()->world().chunks().get(
        world::chunk_coord_for_block(edit), world::local_coord_for_block(edit));
    assert(cell_before && !cell_before.value().is_air());
    input.tick = 2;
    input.sequence = 2;
    assert(session->submit_player_input(input, now_ms));
    assert(session->submit_remove_voxel({edit}, now_ms));
    now_ms += 17;
    auto edited = runtime.run_frame({16'667, now_ms});
    assert(edited);
    assert(!edited.value().server_ticks.empty());
    assert(!edited.value().server_ticks.front().commands.command_reports.empty());
    assert(edited.value().server_ticks.front().commands.command_reports.front().success);

    std::uint32_t hard_corrections = edited.value().client.hard_correction_count;
    std::uint32_t collision_revision_changes =
        edited.value().client.collision_revision_change_count;
    double maximum_correction = edited.value().client.maximum_correction_distance;
    for (std::uint64_t sequence = 3;
         sequence <= 60 && collision_revision_changes == 0; ++sequence) {
        input.tick = sequence;
        input.sequence = sequence;
        assert(session->submit_player_input(input, now_ms));
        now_ms += 17;
        auto frame = runtime.run_frame({16'667, now_ms});
        assert(frame);
        hard_corrections += frame.value().client.hard_correction_count;
        collision_revision_changes += frame.value().client.collision_revision_change_count;
        maximum_correction =
            std::max(maximum_correction, frame.value().client.maximum_correction_distance);
    }

    const auto* after = session->client()->local_player_snapshot();
    assert(after != nullptr);
    assert(after->collision_world_revision > before_collision_revision);
    assert(after->state.yaw_centidegrees == input.yaw_centidegrees);
    assert(collision_revision_changes > 0);
    assert(hard_corrections == 0);
    assert(maximum_correction < 0.05);
    const auto displacement =
        after->state.position.relative_to(before_position.anchor) - before_position.local_offset;
    assert(math::length(displacement) < 0.05);
    assert(runtime.shutdown());
}

void test_boundary_voxel_edit_rebuilds_collision_and_removes_support() {
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.physics_backend = physics::PhysicsBackend::jolt;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);

    std::int64_t now_ms = 0;
    for (std::uint32_t attempt = 0; attempt < 100; ++attempt) {
        const auto* upper = session->server()->chunk_collision().find({0, 0, 0});
        const auto* lower = session->server()->chunk_collision().find({0, -1, 0});
        if (upper != nullptr && lower != nullptr) {
            break;
        }
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto* upper_before = session->server()->chunk_collision().find({0, 0, 0});
    const auto* lower_before = session->server()->chunk_collision().find({0, -1, 0});
    assert(upper_before != nullptr && lower_before != nullptr);
    const auto lower_body = lower_before->body_id;
    const auto collision_revision_before = session->server()->chunk_collision().world_revision();

    const auto client_id = session->client()->client_id();
    auto* player = session->server()->player_for_client(client_id);
    assert(player != nullptr);
    player->state.position = {29.5, 1.0, 7.5};
    player->state.velocity = {};
    player->state.fall_origin = player->state.position;
    player->state.scripted_start = player->state.position;
    player->state.scripted_target = player->state.position;
    player->state.mode = movement::PlayerControllerMode::grounded;
    player->state.grounded = true;

    auto& server_chunks = session->server()->world().chunks();
    server_chunks.clear_all_dirty();
    session->server()->world().dirty_regions().clear();
    const auto upper_address = world::block_to_chunk_local(game::foundation::boundary_edit_upper);
    const auto lower_address = world::block_to_chunk_local(game::foundation::boundary_edit_lower);
    assert(upper_address.chunk == (world::ChunkCoord{0, 0, 0}));
    assert(lower_address.chunk == (world::ChunkCoord{0, -1, 0}));

    movement::PlayerInputFrame neutral;
    neutral.tick = 1;
    neutral.sequence = 1;
    assert(session->submit_player_input(neutral, now_ms));
    assert(session->submit_remove_voxel({game::foundation::boundary_edit_upper}, now_ms));
    now_ms += 17;
    auto edited = runtime.run_frame({16'667, now_ms});
    assert(edited);
    assert(edited.value().server_ticks.front().commands.command_reports.front().success);
    assert(session->client()->accepted_voxel_edits().size() == 1);
    assert(server_chunks.find(upper_address.chunk)->dirty().contains(world::ChunkDirtyFlag::mesh));
    assert(server_chunks.find(lower_address.chunk)->dirty().contains(world::ChunkDirtyFlag::mesh));
    const auto edited_chunk_revision = server_chunks.find(upper_address.chunk)->content_revision();

    bool collision_applied = false;
    bool player_fell = false;
    for (std::uint64_t sequence = 2; sequence <= 120; ++sequence) {
        neutral.tick = sequence;
        neutral.sequence = sequence;
        assert(session->submit_player_input(neutral, now_ms));
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto* upper = session->server()->chunk_collision().find(upper_address.chunk);
        collision_applied = upper != nullptr && upper->content_revision >= edited_chunk_revision;
        player_fell =
            session->server()->player_for_client(client_id)->state.position.approximate_global().y <
            0.1;
        if (collision_applied && player_fell) {
            break;
        }
    }
    assert(collision_applied);
    assert(player_fell);
    const auto* lower_after = session->server()->chunk_collision().find(lower_address.chunk);
    assert(lower_after != nullptr && lower_after->body_id == lower_body);
    assert(session->server()->chunk_collision().world_revision() > collision_revision_before);
    const auto client_cell =
        session->client()->world().chunks().get(upper_address.chunk, upper_address.local);
    assert(client_cell && client_cell.value().is_air());

    player->state.position = {27.5, 1.0, 7.5};
    player->state.velocity = {};
    player->state.mode = movement::PlayerControllerMode::grounded;
    player->state.grounded = true;
    const auto stone = core::PrototypeId::parse("base:voxels/stone");
    assert(stone.has_value());
    assert(session->submit_place_voxel(
        {game::foundation::boundary_edit_upper, *stone}, now_ms));
    now_ms += 17;
    auto replaced = runtime.run_frame({16'667, now_ms});
    assert(replaced);
    assert(replaced.value().server_ticks.front().commands.command_reports.front().success);
    const auto replacement_revision =
        server_chunks.find(upper_address.chunk)->content_revision();
    bool replacement_collision_applied = false;
    for (std::uint64_t sequence = 121; sequence <= 180; ++sequence) {
        neutral.tick = sequence;
        neutral.sequence = sequence;
        assert(session->submit_player_input(neutral, now_ms));
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto* upper = session->server()->chunk_collision().find(upper_address.chunk);
        replacement_collision_applied =
            upper != nullptr && upper->content_revision >= replacement_revision;
        if (replacement_collision_applied) {
            break;
        }
    }
    assert(replacement_collision_applied);

    player = session->server()->player_for_client(client_id);
    player->state.position = {29.5, 1.0, 7.5};
    player->state.velocity = {};
    player->state.fall_origin = player->state.position;
    player->state.mode = movement::PlayerControllerMode::airborne;
    player->state.grounded = false;
    for (std::uint64_t sequence = 181; sequence <= 210; ++sequence) {
        neutral.tick = sequence;
        neutral.sequence = sequence;
        assert(session->submit_player_input(neutral, now_ms));
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
    }
    player = session->server()->player_for_client(client_id);
    assert(player->state.grounded);
    assert(player->state.position.approximate_global().y >= 0.99);
    assert(runtime.shutdown());
}

void test_jolt_runtime_drops_settles_and_restores_physical_resource() {
    if (!physics::physics_backend_info(physics::PhysicsBackend::jolt).available) {
        return;
    }
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.physics_backend = physics::PhysicsBackend::jolt;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr);

    std::int64_t now_ms = 0;
    for (std::uint32_t attempt = 0;
         attempt < 100 && session->server()->chunk_collision().find({0, 0, 0}) == nullptr;
         ++attempt) {
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(session->server()->chunk_collision().find({0, 0, 0}) != nullptr);

    auto resource_id = session->server()->world().save_ids().reserve();
    assert(resource_id);
    entities::PhysicalResourceRecord resource;
    resource.resource_id = resource_id.value();
    resource.prototype_id = *core::PrototypeId::parse("base:entities/dropped_log");
    resource.cargo_prototype_id = *core::PrototypeId::parse("base:cargo/heavy_log");
    resource.position = {9.0, 4.0, 8.5};
    resource.kind = entities::PhysicalResourceKind::haulable_log;
    resource.mass_grams = 12'000;
    resource.volume_milliliters = 24'000;
    resource.allowed_transport_modes = cargo::CargoTransportModes::of(
        {cargo::CargoTransportMode::hand, cargo::CargoTransportMode::cart});
    resource.segments.push_back({physics::ShapeKind::box, {}, {0.6F, 0.2F, 0.2F}, 0.5F, 0.5F});
    assert(session->server()->drop_physical_resource(std::move(resource), {0.4F, 0.0F, 0.0F},
                                                     {0.0F, 0.0F, 0.5F}));

    for (std::uint32_t frame_index = 0; frame_index < 720; ++frame_index) {
        now_ms += 17;
        assert(runtime.run_frame({16'667, now_ms}));
    }
    const auto* settled = session->server()->world().physical_resources().find(resource_id.value());
    assert(settled != nullptr);
    assert(settled->state == entities::PhysicalResourceState::frozen_static);
    assert(settled->position.approximate_global().y > 1.1);
    assert(settled->position.approximate_global().y < 1.9);
    assert(session->server()->physical_resource_physics().stats().frozen_bodies == 1);
    auto snapshot = runtime.capture_save_snapshot();
    assert(snapshot);
    assert(runtime.shutdown());

    auto restored_runtime = make_runtime(report);
    auto restored_request = make_session_request(report);
    restored_request.initial_snapshot = std::move(snapshot).value();
    assert(restored_runtime.start_session(config, std::move(restored_request)));
    auto* restored_server = restored_runtime.session()->server();
    assert(restored_server != nullptr);
    auto restored_frame = restored_runtime.run_frame({16'667, 17});
    assert(restored_frame);
    const auto* restored = restored_server->world().physical_resources().find(resource_id.value());
    assert(restored != nullptr);
    assert(restored->state == entities::PhysicalResourceState::frozen_static);
    assert(restored->physics_body_id.is_valid());
    assert(!restored->needs_physics_rebuild);
    assert(restored_server->physical_resource_physics().stats().restored_bodies == 1);
    assert(restored_runtime.shutdown());
}

void test_authoritative_player_input_moves_and_replicates() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    assert(runtime.start_session(config, make_session_request(report)));

    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto initial_floor = session->client()->world().chunks().get({0, 0, 0}, {8, 0, 8});
    assert(initial_floor && !initial_floor.value().is_air());
    const auto client_id = session->client()->client_id();
    const auto* player_before = session->server()->player_for_client(client_id);
    assert(player_before != nullptr);
    const auto player_net_id = player_before->net_id;
    assert(session->client()->local_player_net_id() == player_net_id);
    assert(session->client()->local_player_snapshot() != nullptr);
    const auto start = player_before->state.position;

    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    input.yaw_centidegrees = 4'500;
    input.pitch_centidegrees = 7'000;
    assert(session->submit_player_input(input, 10));
    const auto* prediction_diagnostics = session->client()->last_prediction_diagnostics();
    assert(prediction_diagnostics != nullptr);
    assert(prediction_diagnostics->requested_displacement.z > 0.0);
    assert(prediction_diagnostics->applied_displacement.z > 0.0);
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame);
    assert(frame.value().server_ticks.size() == 1);
    const auto& tick = frame.value().server_ticks.front();
    assert(tick.commands.command_reports.empty());
    assert(tick.commands.control_message_count == 1);
    assert(tick.accepted_movement_input_count == 1);
    assert(tick.rejected_movement_input_count == 0);
    assert(tick.moved_player_count == 1);
    assert(tick.movement_snapshot_count == 1);
    assert(frame.value().client.predicted_input_count == 1);
    assert(frame.value().client.acknowledged_input_count == 1);
    assert(frame.value().client.hard_correction_count == 0);
    assert(session->server()->events().character_moved.size() == 1);

    const auto* player_after = session->server()->player_for_client(client_id);
    assert(player_after != nullptr);
    assert(player_after->state.position.relative_to(start.anchor).z > start.local_offset.z);
    const auto* authoritative_entity =
        session->server()->world().entities().find(player_after->runtime_handle);
    assert(authoritative_entity != nullptr);
    assert(authoritative_entity->transform.rotation_degrees.x == 0.0);
    assert(authoritative_entity->transform.rotation_degrees.y == 45.0);
    assert(authoritative_entity->transform.rotation_degrees.z == 0.0);
    const auto* snapshot = session->client()->player_snapshot(player_net_id);
    assert(snapshot != nullptr);
    assert(snapshot->state.position == player_after->state.position);
    assert(snapshot->last_processed_input_sequence == 1);
    auto render_snapshot = runtime.capture_render_snapshot();
    assert(render_snapshot);
    assert(render_snapshot.value().objects.size() == 2);
    const auto player_object = std::ranges::find(
        render_snapshot.value().objects, player_net_id,
        [](const game::RenderObjectSnapshot& object) { return object.source_net_id; });
    assert(player_object != render_snapshot.value().objects.end());
    assert(player_object->current_transform.position == player_after->state.position);
    assert(player_object->previous_transform.position == start);
    assert(player_object->current_transform.rotation_degrees.x == 0.0);
    assert(player_object->current_transform.rotation_degrees.y == 45.0);
    assert(player_object->current_transform.rotation_degrees.z == 0.0);
    assert(std::ranges::any_of(render_snapshot.value().objects, [](const auto& object) {
        return object.visual_prototype.value() ==
               "base:entities/foundation_material_showcase";
    }));

    auto repeated = runtime.run_frame({16'667, 34});
    assert(repeated && repeated.value().server_ticks.size() == 1);
    assert(repeated.value().server_ticks.front().repeated_input_count == 1);
    assert(repeated.value().server_ticks.front().movement_snapshot_count == 1);
    const auto* repeated_player = session->server()->player_for_client(client_id);
    assert(repeated_player != nullptr);
    assert(repeated_player->state.last_input_sequence == 1);
    assert(repeated_player->state.simulation_tick == 2);
    assert(runtime.shutdown());
}

void test_transient_replication_budget_defers_latest_state() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.max_transient_snapshot_payload_bytes_per_tick = 1;
    assert(runtime.start_session(config, make_session_request(report)));
    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    assert(runtime.session()->submit_player_input(input, 10));
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame && frame.value().server_ticks.size() == 1);
    assert(frame.value().server_ticks.front().movement_snapshot_count == 0);
    assert(frame.value().server_ticks.front().deferred_transient_snapshot_count == 1);
    assert(frame.value().server_ticks.front().transient_snapshot_payload_bytes == 0);
    assert(frame.value().client.acknowledged_input_count == 0);
    assert(runtime.shutdown());
}

void test_prediction_under_deterministic_100ms_rtt_and_two_percent_loss() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.fixed_step = {60, 4, 250'000};
    config.simulated_network_one_way_latency_ms = 50;
    config.simulated_network_jitter_ms = 10;
    config.simulated_network_unreliable_loss_basis_points = 200;
    config.simulated_network_seed = 0x123456789abcdef0ULL;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);

    std::int64_t now_ms = 0;
    for (std::uint32_t frame_index = 0; frame_index < 60; ++frame_index) {
        now_ms += 17;
        auto frame = runtime.run_frame({16'667, now_ms});
        assert(frame);
        if (session->client()->is_connected() &&
            session->client()->local_player_snapshot() != nullptr) {
            break;
        }
    }
    assert(session->client()->is_connected());
    const auto* initial_snapshot = session->client()->local_player_snapshot();
    assert(initial_snapshot != nullptr);
    const auto initial_position = initial_snapshot->state.position;

    std::uint64_t server_to_client_bytes = 0;
    std::uint64_t one_second_window_bytes = 0;
    std::uint64_t peak_one_second_bytes = 0;
    std::uint32_t simulated_drops = 0;
    std::uint32_t accepted_inputs = 0;
    std::uint32_t hard_corrections = 0;
    double maximum_correction_distance = 0.0;
    constexpr std::uint32_t measured_ticks = 600;
    for (std::uint32_t tick = 1; tick <= measured_ticks; ++tick) {
        movement::PlayerInputFrame input;
        input.tick = tick;
        input.sequence = tick;
        input.move_z = 32'767;
        assert(session->submit_player_input(input, now_ms));
        if (tick == 1) {
            const auto* immediate = session->client()->local_player_snapshot();
            assert(immediate != nullptr);
            assert(immediate->state.position.relative_to(initial_position.anchor).z >
                   initial_position.local_offset.z);
        }

        now_ms += 17;
        auto frame = runtime.run_frame({16'667, now_ms});
        assert(frame && !frame.value().server_ticks.empty());
        for (const auto& server_tick : frame.value().server_ticks) {
            server_to_client_bytes += server_tick.commands.transport_server_to_client_bytes;
            one_second_window_bytes += server_tick.commands.transport_server_to_client_bytes;
            simulated_drops +=
                server_tick.commands.transport_simulated_dropped_unreliable_message_count;
            accepted_inputs += server_tick.accepted_movement_input_count;
        }
        hard_corrections += frame.value().client.hard_correction_count;
        maximum_correction_distance =
            std::max(maximum_correction_distance, frame.value().client.maximum_correction_distance);
        if (tick % 60U == 0) {
            peak_one_second_bytes = std::max(peak_one_second_bytes, one_second_window_bytes);
            one_second_window_bytes = 0;
        }
    }

    const auto measured_seconds =
        static_cast<double>(measured_ticks) / config.fixed_step.ticks_per_second;
    const auto average_bytes_per_second =
        static_cast<double>(server_to_client_bytes) / measured_seconds;
    assert(simulated_drops > 0);
    assert(accepted_inputs > measured_ticks * 9U / 10U);
    // A collision-world revision change may intentionally flush prediction history once during
    // bootstrap; its correction must still remain below the published 1 m comfort threshold.
    assert(hard_corrections <= 1);
    assert(maximum_correction_distance < 1.0);
    assert(average_bytes_per_second < 64.0 * 1024.0);
    assert(peak_one_second_bytes < 256U * 1024U);
    const auto client_id = session->client()->client_id();
    const auto* authoritative = session->server()->player_for_client(client_id);
    assert(authoritative != nullptr);
    assert(authoritative->state.position.relative_to(initial_position.anchor).z >
           initial_position.local_offset.z + 5.0);
    assert(runtime.shutdown());
}

void test_typed_voxel_commands_validate_and_replicate() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    assert(runtime.start_session({}, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    const auto lighting_regions_before =
        session->server()->chunk_lighting().stats().dirty_regions_consumed;
    const auto fluid_regions_before =
        session->server()->chunk_fluids().stats().dirty_regions_consumed;
    const auto clay = core::PrototypeId::parse("base:voxels/clay");
    const auto raw_clay = core::PrototypeId::parse("base:items/raw_clay");
    assert(clay.has_value());
    assert(raw_clay.has_value());
    const auto item_count = [](const world::InventoryRecord& inventory,
                               const core::PrototypeId& prototype_id) {
        std::uint32_t count = 0;
        for (const auto& stack : inventory.stacks) {
            if (stack.prototype_id == prototype_id) {
                count += stack.count;
            }
        }
        return count;
    };
    auto* player = session->server()->player_for_client(session->client()->client_id());
    assert(player != nullptr);
    const auto* starting_inventory =
        session->server()->world().inventories().find(player->save_id);
    assert(starting_inventory != nullptr);
    const auto starting_raw_clay = item_count(*starting_inventory, *raw_clay);
    const game::interaction::PlaceVoxelCommand place{{9, 1, 8}, *clay};
    assert(session->submit_place_voxel(place, 10));
    auto placed = runtime.run_frame({16'667, 17});
    assert(placed && placed.value().server_ticks.size() == 1);
    assert(placed.value().server_ticks.front().commands.command_reports.front().success);
    const auto& placed_trace =
        placed.value().server_ticks.front().commands.command_reports.front().operation_trace;
    assert(std::ranges::find(placed_trace.derived_updates, "chunk_lighting") !=
           placed_trace.derived_updates.end());
    assert(std::ranges::find(placed_trace.derived_updates, "voxel_fluids") !=
           placed_trace.derived_updates.end());
    assert(session->server()->chunk_lighting().stats().dirty_regions_consumed >
           lighting_regions_before);
    assert(session->server()->chunk_fluids().stats().dirty_regions_consumed > fluid_regions_before);
    assert(session->server()->events().voxel_changed.size() == 1);
    const auto address = world::block_to_chunk_local(place.position);
    auto authoritative = session->server()->world().chunks().get(address.chunk, address.local);
    auto replicated = session->client()->world().chunks().get(address.chunk, address.local);
    assert(authoritative && replicated);
    assert(!authoritative.value().is_air());
    assert(replicated.value() == authoritative.value());
    assert(placed.value().client.replication.total_applied_record_count == 1);
    assert(session->client()->accepted_voxel_edits().size() == 1);
    assert(session->client()->accepted_voxel_edits().front().position == place.position);
    assert(session->client()->accepted_voxel_edits().front().current == replicated.value());

    assert(session->submit_place_voxel(place, 20));
    auto duplicate = runtime.run_frame({16'667, 34});
    assert(duplicate);
    assert(!duplicate.value().server_ticks.front().commands.command_reports.front().success);
    assert(duplicate.value().server_ticks.front().commands.command_reports.front().error_code ==
           "voxel_command.target_occupied");
    assert(session->client()->accepted_voxel_edits().empty());

    const game::interaction::PlaceVoxelCommand far_away{{100, 1, 100}, *clay};
    assert(session->submit_place_voxel(far_away, 30));
    auto rejected = runtime.run_frame({16'667, 51});
    assert(rejected);
    assert(!rejected.value().server_ticks.front().commands.command_reports.front().success);
    assert(rejected.value().server_ticks.front().commands.command_reports.front().error_code ==
           "voxel_command.out_of_reach");
    assert(session->client()->accepted_voxel_edits().empty());

    const game::interaction::PlaceVoxelCommand inside_player{{8, 1, 8}, *clay};
    assert(session->submit_place_voxel(inside_player, 40));
    auto intersecting = runtime.run_frame({16'667, 68});
    assert(intersecting);
    assert(!intersecting.value().server_ticks.front().commands.command_reports.front().success);
    assert(intersecting.value().server_ticks.front().commands.command_reports.front().error_code ==
           "voxel_command.intersects_player");
    assert(session->client()->accepted_voxel_edits().empty());

    assert(session->submit_remove_voxel({place.position}, 50));
    auto removed = runtime.run_frame({16'667, 85});
    assert(removed);
    authoritative = session->server()->world().chunks().get(address.chunk, address.local);
    replicated = session->client()->world().chunks().get(address.chunk, address.local);
    assert(authoritative && authoritative.value().is_air());
    assert(replicated && replicated.value().is_air());
    assert(session->client()->accepted_voxel_edits().size() == 1);
    assert(!session->client()->accepted_voxel_edits().front().previous.is_air());
    assert(session->client()->accepted_voxel_edits().front().current.is_air());
    const auto& removal_report =
        removed.value().server_ticks.front().commands.command_reports.front();
    assert(removal_report.success);
    assert(removal_report.events.size() == 2);
    assert(std::ranges::any_of(removal_report.events, [](const auto& event) {
        return event.type == game::interaction::voxel_resource_granted_event_type;
    }));
    const auto* authoritative_inventory =
        session->server()->world().inventories().find(player->save_id);
    const auto* replicated_inventory =
        session->client()->world().inventories().find(player->save_id);
    assert(authoritative_inventory != nullptr && replicated_inventory != nullptr);
    assert(item_count(*authoritative_inventory, *raw_clay) == starting_raw_clay + 1);
    assert(item_count(*replicated_inventory, *raw_clay) == starting_raw_clay + 1);

    assert(session->submit_remove_voxel({place.position}, 60));
    auto duplicate_removal = runtime.run_frame({16'667, 102});
    assert(duplicate_removal);
    assert(
        !duplicate_removal.value().server_ticks.front().commands.command_reports.front().success);
    assert(duplicate_removal.value()
               .server_ticks.front()
               .commands.command_reports.front()
               .error_code == "voxel_command.target_empty");
    assert(session->client()->accepted_voxel_edits().empty());
    authoritative_inventory =
        session->server()->world().inventories().find(player->save_id);
    assert(authoritative_inventory != nullptr);
    assert(item_count(*authoritative_inventory, *raw_clay) == starting_raw_clay + 1);

    player = session->server()->player_for_client(session->client()->client_id());
    assert(player != nullptr);
    player->state.position = {31.5, 1.0, 31.5};
    const game::interaction::PlaceVoxelCommand unloaded{{32, 1, 31}, *clay};
    assert(session->submit_place_voxel(unloaded, 70));
    auto unloaded_edit = runtime.run_frame({16'667, 119});
    assert(unloaded_edit);
    assert(!unloaded_edit.value().server_ticks.front().commands.command_reports.front().success);
    assert(unloaded_edit.value().server_ticks.front().commands.command_reports.front().error_code ==
           "voxel_command.chunk_not_loaded");
    assert(session->client()->accepted_voxel_edits().empty());
    assert(runtime.shutdown());
}

void test_runtime_relights_and_replicates_chunk_light() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.chunk_lighting.max_snapshot_cells_per_update = world::VoxelChunk::total_cells;
    config.chunk_lighting.apply_time_budget_ms = 20.0;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto& server_world = session->server()->world();
    const auto clay_type =
        report.voxel_palette.type_for(*core::PrototypeId::parse("base:voxels/clay"));
    assert(clay_type.has_value());
    const world::VoxelCell clay{*clay_type, 0};
    constexpr world::BlockCoord center{12, 2, 12};
    constexpr std::array<world::BlockCoord, 6> enclosure{{
        {11, 2, 12},
        {13, 2, 12},
        {12, 1, 12},
        {12, 3, 12},
        {12, 2, 11},
        {12, 2, 13},
    }};
    for (const auto position : enclosure) {
        const auto address = world::block_to_chunk_local(position);
        assert(server_world.chunks().set(address.chunk, address.local, clay,
                                         server_world.dirty_regions(), report.voxel_palette));
    }

    auto wait_for_relight = [&](std::uint64_t target) {
        for (std::size_t frame_index = 0;
             frame_index < 5'000 &&
             session->server()->chunk_lighting().stats().applied_fields < target;
             ++frame_index) {
            auto frame =
                runtime.run_frame({16'667, static_cast<std::int64_t>((frame_index + 1) * 17)});
            assert(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(session->server()->chunk_lighting().stats().applied_fields >= target);
    };

    wait_for_relight(1);
    const auto center_address = world::block_to_chunk_local(center);
    auto authoritative = server_world.chunks().get(center_address.chunk, center_address.local);
    auto replicated =
        session->client()->world().chunks().get(center_address.chunk, center_address.local);
    assert(authoritative && replicated);
    assert(authoritative.value().light == 0);
    assert(replicated.value().light == authoritative.value().light);

    const auto roof_address = world::block_to_chunk_local(enclosure[3]);
    assert(server_world.chunks().set(roof_address.chunk, roof_address.local,
                                     world::VoxelCell::air(), server_world.dirty_regions(),
                                     report.voxel_palette));
    wait_for_relight(2);
    authoritative = server_world.chunks().get(center_address.chunk, center_address.local);
    replicated =
        session->client()->world().chunks().get(center_address.chunk, center_address.local);
    assert(authoritative && replicated);
    assert(authoritative.value().light == 255);
    assert(replicated.value().light == authoritative.value().light);
    assert(session->server()->chunk_lighting().stats().total_changed_chunks >= 2);
    assert(runtime.shutdown());
}

void test_runtime_simulates_and_replicates_voxel_fluid() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    game::RuntimeConfiguration config;
    config.chunk_fluids.simulation_tick_interval = 1;
    config.chunk_fluids.maximum_active_cells_per_step = world::VoxelChunk::total_cells;
    config.chunk_fluids.apply_time_budget_ms = 20.0;
    assert(runtime.start_session(config, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto& server_world = session->server()->world();
    const auto water_type =
        report.voxel_palette.type_for(*core::PrototypeId::parse("base:voxels/water"));
    assert(water_type.has_value());

    constexpr world::BlockCoord source_position{12, 1, 12};
    constexpr world::BlockCoord flow_position{13, 1, 12};
    const auto source = world::block_to_chunk_local(source_position);
    const auto flow = world::block_to_chunk_local(flow_position);
    const world::VoxelCell water_source{*water_type, 0, world::full_fluid_source_state_bits()};
    assert(server_world.chunks().set(source.chunk, source.local, water_source,
                                     server_world.dirty_regions(), report.voxel_palette));
    for (std::size_t frame_index = 0; frame_index < 12; ++frame_index) {
        auto frame = runtime.run_frame({16'667, static_cast<std::int64_t>((frame_index + 1) * 17)});
        assert(frame);
    }

    auto authoritative = server_world.chunks().get(flow.chunk, flow.local);
    auto replicated = session->client()->world().chunks().get(flow.chunk, flow.local);
    assert(authoritative && replicated && authoritative.value().type == *water_type);
    assert(replicated.value().type == authoritative.value().type);
    assert(replicated.value().state_bits == authoritative.value().state_bits);
    auto state = world::decode_fluid_state(authoritative.value().state_bits);
    assert(state && state.value().amount == 7);
    assert(session->server()->chunk_fluids().stats().steps > 0);
    assert(session->server()->chunk_fluids().stats().total_changed_cells > 0);

    assert(server_world.chunks().set(source.chunk, source.local, world::VoxelCell::air(),
                                     server_world.dirty_regions(), report.voxel_palette));
    for (std::size_t frame_index = 0; frame_index < 12; ++frame_index) {
        auto frame =
            runtime.run_frame({16'667, static_cast<std::int64_t>((frame_index + 20) * 17)});
        assert(frame);
    }
    authoritative = server_world.chunks().get(flow.chunk, flow.local);
    replicated = session->client()->world().chunks().get(flow.chunk, flow.local);
    assert(authoritative && authoritative.value().is_air());
    assert(replicated && replicated.value().is_air());
    assert(runtime.shutdown());
}

void test_session_save_and_reload_restores_authoritative_state() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    auto request = make_session_request(report);
    assert(runtime.start_session({}, request));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);

    const auto clay = core::PrototypeId::parse("base:voxels/clay");
    assert(clay.has_value());
    const game::interaction::PlaceVoxelCommand placed_voxel{{10, 1, 8}, *clay};
    constexpr world::BlockCoord removed_voxel{8, 0, 8};
    assert(session->submit_place_voxel(placed_voxel, 10));
    assert(session->submit_remove_voxel({removed_voxel}, 11));
    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    assert(session->submit_player_input(input, 10));
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame && frame.value().server_ticks.size() == 1);
    assert(frame.value().server_ticks.front().commands.command_reports.size() == 2);
    assert(std::ranges::all_of(frame.value().server_ticks.front().commands.command_reports,
                               [](const auto& command) { return command.success; }));
    const auto removed_address = world::block_to_chunk_local(removed_voxel);
    const auto removed_authoritative =
        session->server()->world().chunks().get(removed_address.chunk, removed_address.local);
    assert(removed_authoritative && removed_authoritative.value().is_air());
    auto* saved_player = session->server()->player_for_client(session->client()->client_id());
    assert(saved_player != nullptr);
    saved_player->state.velocity = {1.25, -2.5, 0.75};
    saved_player->state.mode = movement::PlayerControllerMode::airborne;
    saved_player->state.grounded = false;
    saved_player->state.crouched = true;
    saved_player->state.health_milli = 73'000;
    saved_player->state.stamina_milli = 21'000;
    saved_player->state.yaw_centidegrees = 12'345;
    saved_player->state.pitch_centidegrees = -2'500;
    const auto saved_player_state = saved_player->state;
    const auto saved_fixed_step_tick = session->fixed_step_tick();
    const auto grass_tuft = core::PrototypeId::parse("base:items/grass_tuft");
    assert(grass_tuft.has_value());
    const auto player_save_id =
        session->server()->player_for_client(session->client()->client_id())->save_id;
    const auto* saved_inventory =
        session->server()->world().inventories().find(player_save_id);
    assert(saved_inventory != nullptr);
    assert(std::ranges::any_of(saved_inventory->stacks, [&](const auto& stack) {
        return stack.prototype_id == *grass_tuft && stack.count == 1;
    }));

    const auto save_root =
        std::filesystem::temp_directory_path() / "heartstead-runtime-save-reload-test";
    std::filesystem::remove_all(save_root);
    const save::FileSaveDatabase database(save_root);
    assert(runtime.save_to(database));
    auto persisted = database.read_snapshot();
    assert(persisted);
    assert(!persisted.value().chunk_edits.empty());
    assert(!persisted.value().entities.empty());
    const auto persisted_player = std::ranges::find(
        persisted.value().entities, entities::EntityKind::player,
        &save::EntitySaveRecord::kind);
    assert(persisted_player != persisted.value().entities.end());
    assert(persisted_player->encoded_state.starts_with(
        movement::player_controller_save_state_magic));
    assert(std::ranges::any_of(persisted.value().mod_states, [&](const auto& state) {
        return state.mod_id == "engine" && state.state_key == "runtime.fixed_step_tick" &&
               state.encoded_state == std::to_string(saved_fixed_step_tick);
    }));
    assert(persisted.value().voxel_palette.entries == report.voxel_palette.manifest().entries);
    assert(runtime.shutdown());

    game::RuntimeConfiguration headless_config;
    headless_config.create_client = false;
    headless_config.headless = true;
    assert(runtime.start_session_from_save(headless_config, database));
    session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() == nullptr);
    assert(session->server()->world().voxel_palette_manifest().entries ==
           persisted.value().voxel_palette.entries);
    assert(session->server()->voxel_palette().manifest().entries ==
           persisted.value().voxel_palette.entries);
    const auto* restored_inventory =
        session->server()->world().inventories().find(player_save_id);
    assert(restored_inventory != nullptr);
    assert(std::ranges::any_of(restored_inventory->stacks, [&](const auto& stack) {
        return stack.prototype_id == *grass_tuft && stack.count == 1;
    }));
    const auto address = world::block_to_chunk_local(placed_voxel.position);
    const auto headless_authoritative =
        session->server()->world().chunks().get(address.chunk, address.local);
    assert(headless_authoritative && !headless_authoritative.value().is_air());
    const auto headless_removed =
        session->server()->world().chunks().get(removed_address.chunk, removed_address.local);
    assert(headless_removed && headless_removed.value().is_air());
    auto headless_snapshot = runtime.capture_save_snapshot();
    assert(headless_snapshot && !headless_snapshot.value().chunk_edits.empty());
    assert(runtime.shutdown());

    assert(runtime.start_session_from_save({}, database));
    session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    assert(session->fixed_step_tick() == saved_fixed_step_tick);
    const auto authoritative =
        session->server()->world().chunks().get(address.chunk, address.local);
    const auto replicated = session->client()->world().chunks().get(address.chunk, address.local);
    assert(authoritative && replicated);
    assert(!authoritative.value().is_air());
    assert(replicated.value() == authoritative.value());
    const auto authoritative_removed =
        session->server()->world().chunks().get(removed_address.chunk, removed_address.local);
    const auto replicated_removed =
        session->client()->world().chunks().get(removed_address.chunk, removed_address.local);
    assert(authoritative_removed && authoritative_removed.value().is_air());
    assert(replicated_removed && replicated_removed.value().is_air());
    const auto* restored_player =
        session->server()->player_for_client(session->client()->client_id());
    assert(restored_player != nullptr);
    assert(restored_player->state.position == saved_player_state.position);
    assert(restored_player->state.velocity == saved_player_state.velocity);
    assert(restored_player->state.mode == saved_player_state.mode);
    assert(restored_player->state.grounded == saved_player_state.grounded);
    assert(restored_player->state.crouched == saved_player_state.crouched);
    assert(restored_player->state.health_milli == saved_player_state.health_milli);
    assert(restored_player->state.stamina_milli == saved_player_state.stamina_milli);
    assert(restored_player->state.yaw_centidegrees == saved_player_state.yaw_centidegrees);
    assert(restored_player->state.pitch_centidegrees == saved_player_state.pitch_centidegrees);
    assert(session->client()->local_player_snapshot() != nullptr);
    assert(session->client()->local_player_snapshot()->state.position == saved_player_state.position);
    assert(runtime.shutdown());
    std::filesystem::remove_all(save_root);
}

void test_boundary_voxel_edits_survive_restart() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    assert(runtime.start_session({}, make_session_request(report)));
    auto* session = runtime.session();
    assert(session != nullptr && session->server() != nullptr && session->client() != nullptr);
    auto* player = session->server()->player_for_client(session->client()->client_id());
    assert(player != nullptr);
    player->state.position = {29.5, 1.0, 7.5};
    player->state.velocity = {};
    player->state.grounded = true;
    player->state.mode = movement::PlayerControllerMode::grounded;

    assert(session->submit_remove_voxel({game::foundation::boundary_edit_upper}, 10));
    assert(session->submit_remove_voxel({game::foundation::boundary_edit_lower}, 11));
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame && frame.value().server_ticks.size() == 1);
    assert(frame.value().server_ticks.front().commands.command_reports.size() == 2);
    assert(std::ranges::all_of(frame.value().server_ticks.front().commands.command_reports,
                               [](const auto& command) { return command.success; }));

    const auto save_root =
        std::filesystem::temp_directory_path() / "heartstead-boundary-save-reload-test";
    std::filesystem::remove_all(save_root);
    const save::FileSaveDatabase database(save_root);
    assert(runtime.save_to(database));
    auto persisted = database.read_snapshot();
    assert(persisted);
    const auto upper_chunk = world::chunk_coord_for_block(game::foundation::boundary_edit_upper);
    const auto lower_chunk = world::chunk_coord_for_block(game::foundation::boundary_edit_lower);
    assert(upper_chunk != lower_chunk);
    assert(std::ranges::any_of(persisted.value().chunk_edits,
                               [&](const auto& edit) { return edit.coord == upper_chunk; }));
    assert(std::ranges::any_of(persisted.value().chunk_edits,
                               [&](const auto& edit) { return edit.coord == lower_chunk; }));
    assert(runtime.shutdown());

    game::RuntimeConfiguration headless;
    headless.create_client = false;
    headless.headless = true;
    assert(runtime.start_session_from_save(headless, database));
    session = runtime.session();
    assert(session != nullptr && session->server() != nullptr);
    for (const auto position :
         {game::foundation::boundary_edit_upper, game::foundation::boundary_edit_lower}) {
        const auto address = world::block_to_chunk_local(position);
        const auto cell = session->server()->world().chunks().get(address.chunk, address.local);
        assert(cell && cell.value().is_air());
    }
    auto reloaded_snapshot = runtime.capture_save_snapshot();
    assert(reloaded_snapshot);
    assert(std::ranges::any_of(reloaded_snapshot.value().chunk_edits,
                               [&](const auto& edit) { return edit.coord == upper_chunk; }));
    assert(std::ranges::any_of(reloaded_snapshot.value().chunk_edits,
                               [&](const auto& edit) { return edit.coord == lower_chunk; }));
    assert(runtime.shutdown());
    std::filesystem::remove_all(save_root);
}

void test_foundation_save_rejects_incompatible_layout() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    assert(runtime.start_session({}, make_session_request(report)));
    auto snapshot = runtime.capture_save_snapshot();
    assert(snapshot);
    assert(runtime.shutdown());

    auto layout = std::ranges::find_if(
        snapshot.value().mod_states, [](const save::ModStateSaveRecord& record) {
            return record.mod_id == game::foundation::layout_state_mod &&
                   record.state_key == game::foundation::layout_state_key;
        });
    assert(layout != snapshot.value().mod_states.end());
    layout->encoded_state = std::to_string(game::foundation::layout_version + 1);

    auto request = make_session_request(report);
    request.initial_snapshot = std::move(snapshot).value();
    const auto status = runtime.start_session({}, std::move(request));
    assert(!status);
    assert(status.error().code == "foundation_world.layout_version_mismatch");
    assert(status.error().message.find("current layout version") != std::string::npos);
}

void test_session_file_load_preserves_missing_prototypes() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);

    save::SaveSnapshot snapshot;
    snapshot.metadata = make_session_request(report).metadata;
    snapshot.mod_states.push_back(
        {"engine", "scenario.id", std::string(game::foundation::scenario_id)});
    snapshot.mod_states.push_back({std::string(game::foundation::layout_state_mod),
                                   std::string(game::foundation::layout_state_key),
                                   std::to_string(game::foundation::layout_version)});
    build::BuildPieceRecord removed_build_piece;
    removed_build_piece.object_id = core::SaveId::from_value(77);
    removed_build_piece.prototype_id =
        core::PrototypeId::parse("removed:build_pieces/legacy_workbench").value();
    snapshot.build_pieces.push_back(removed_build_piece);

    const auto save_root =
        std::filesystem::temp_directory_path() / "heartstead-runtime-missing-prototype-test";
    std::filesystem::remove_all(save_root);
    const save::FileSaveDatabase database(save_root);
    assert(database.write_snapshot(snapshot));

    game::RuntimeConfiguration config;
    config.create_client = false;
    config.headless = true;
    assert(
        runtime.start_session_from_save(std::move(config), database, "base:scenarios/homestead"));
    const auto* server = runtime.session()->server();
    assert(server != nullptr);
    assert(server->world().build_objects().count() == 0);
    assert(server->world().missing_prototypes().size() == 1);
    assert(server->world().missing_prototypes().front().original_prototype_id ==
           removed_build_piece.prototype_id);
    assert(runtime.shutdown());
    std::filesystem::remove_all(save_root);
}

void test_session_load_restores_persisted_missing_voxel_palette() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    assert(runtime.start_session({}, make_session_request(report)));
    auto snapshot = runtime.capture_save_snapshot();
    assert(snapshot);
    assert(runtime.shutdown());

    assert(!snapshot.value().voxel_palette.entries.empty());
    const auto highest_type = std::ranges::max(snapshot.value().voxel_palette.entries, {},
                                               &world::VoxelPaletteManifestEntry::type);
    assert(highest_type.type < std::numeric_limits<std::uint16_t>::max());
    const auto missing_type = static_cast<std::uint16_t>(highest_type.type + 1U);
    const auto missing_id = core::PrototypeId::parse("removed:voxels/legacy_masonry");
    assert(missing_id);
    snapshot.value().voxel_palette.entries.push_back({missing_type, *missing_id});

    constexpr world::BlockCoord missing_position{10, 1, 8};
    const auto address = world::block_to_chunk_local(missing_position);
    world::VoxelEditRecord missing_edit{
        address.chunk, address.local, {}, world::VoxelCell{missing_type, 0}};
    const std::vector<const world::VoxelEditRecord*> edits{&missing_edit};
    snapshot.value().chunk_edits.push_back(
        {address.chunk, world::ChunkEditDeltaTextCodec::encode(address.chunk, edits)});

    const auto save_root =
        std::filesystem::temp_directory_path() / "heartstead-runtime-missing-voxel-test";
    std::filesystem::remove_all(save_root);
    const save::FileSaveDatabase database(save_root);
    assert(database.write_snapshot(snapshot.value()));

    game::RuntimeConfiguration headless;
    headless.create_client = false;
    headless.headless = true;
    assert(runtime.start_session_from_save(headless, database));
    const auto* server = runtime.session()->server();
    assert(server != nullptr);
    const auto* missing = server->voxel_palette().find_by_type(missing_type);
    assert(missing != nullptr);
    assert(missing->prototype_id == *missing_id);
    assert(missing->missing_prototype);
    assert(missing->terrain_material == "missing");
    assert(missing->display_name.find(missing_id->value()) != std::string::npos);
    const auto restored = server->world().chunks().get(address.chunk, address.local);
    assert(restored && restored.value().type == missing_type);
    auto round_trip = runtime.capture_save_snapshot();
    assert(round_trip);
    const auto* persisted_missing = round_trip.value().voxel_palette.find_by_type(missing_type);
    assert(persisted_missing != nullptr && persisted_missing->prototype_id == *missing_id);
    assert(runtime.shutdown());
    std::filesystem::remove_all(save_root);
}

void test_gameplay_modules_extend_runtime_through_registration_contract() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    auto module = std::make_shared<TestGameplayModule>();
    game::RuntimeConfiguration config;
    config.gameplay_modules.push_back(module);
    auto request = make_session_request(report);
    request.metadata.enabled_mods.push_back({"test", "0.0.1", "test-feature"});
    assert(runtime.start_session(config, std::move(request)));
    const auto* server = runtime.session()->server();
    assert(server != nullptr);
    const auto& module_report = server->gameplay_modules().report();
    assert(module_report.module_ids.size() == 2);
    assert(module_report.module_ids.front() == "base.voxel_interaction");
    assert(module_report.module_ids.back() == "test.feature");
    assert(module_report.component_count == 1);
    assert(module_report.service_count == 1);
    assert(module_report.command_count == 3);
    assert(module_report.system_count == 1);
    assert(module_report.serializer_count == 3);
    assert(module_report.persistence_count == 1);
    assert(module_report.replication_count == 3);
    assert(module_report.presentation_adapter_count == 1);
    const auto* service = server->domain_services().find<ITestFeatureService>();
    assert(service != nullptr && service->value() == 42);
    assert(runtime.submit_command("test.feature.set", {}, 10));
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame && frame.value().server_ticks.size() == 1);
    assert(frame.value().server_ticks.front().commands.command_reports.size() == 1);
    assert(frame.value().server_ticks.front().commands.command_reports.front().success);
    assert(module->update_count == 1);
    assert(frame.value()
               .server_ticks.front()
               .commands.command_reports.front()
               .committed_world_mutation);
    assert(frame.value().client.feature_replication.callback_event_count == 1);
    assert(frame.value().client.feature_replication.unhandled_event_count == 0);
    assert(frame.value().presentation.adapter_count == 2);
    assert(frame.value().presentation.inserted_objects == 1);
    assert(module->client_visible && module->client_revision == 1);
    assert(server->world().mod_states().find("test", "feature_visible") != nullptr);
    const auto render_snapshot = runtime.capture_render_snapshot();
    assert(render_snapshot && render_snapshot.value().objects.size() == 3);
    assert(runtime.session()->presentation()->find_object(core::NetId::from_value(9'001)) !=
           nullptr);
    auto save_snapshot = runtime.capture_save_snapshot();
    assert(save_snapshot);
    assert(std::ranges::any_of(save_snapshot.value().mod_states, [](const auto& record) {
        return record.mod_id == "test" && record.state_key == "feature_visible" &&
               record.encoded_state == "true";
    }));
    assert(std::ranges::any_of(save_snapshot.value().mod_states, [](const auto& record) {
        return record.mod_id == "engine" && record.state_key == "scenario.id" &&
               record.encoded_state == "base:scenarios/homestead";
    }));
    assert(module->persistence_capture_count == 1);
    const auto session_diagnostics = game::GameInspector::inspect(*runtime.session());
    assert(session_diagnostics.state == "running");
    assert(session_diagnostics.find_field("authoritative_world_tick") != nullptr);
    assert(session_diagnostics.find_field("loaded_chunk_count") != nullptr);
    assert(session_diagnostics.find_field("live_entity_count") != nullptr);
    assert(session_diagnostics.find_field("client_pending_command_count") != nullptr);
    assert(session_diagnostics.find_field("presentation_object_count") != nullptr);
    const auto frame_diagnostics = game::GameInspector::inspect(frame.value());
    assert(frame_diagnostics.find_field("simulation_ms") != nullptr);
    assert(frame_diagnostics.find_field("replication_message_count") != nullptr);
    const auto client_diagnostics = game::GameInspector::inspect(frame.value().client);
    assert(client_diagnostics.find_field("feature_replication_callback_count")->value == "1");
    const auto presentation_diagnostics = game::GameInspector::inspect(frame.value().presentation);
    assert(presentation_diagnostics.find_field("adapter_count")->value == "2");
    const auto module_diagnostics = game::GameInspector::inspect(module_report);
    assert(module_diagnostics.find_field("module_count")->value == "2");
    const auto timing_diagnostics = game::GameInspector::inspect_system_timings(*server);
    assert(timing_diagnostics.size() == server->scheduler().registered_system_count());
    assert(std::ranges::all_of(timing_diagnostics, [](const auto& timing) {
        const auto* invocation_count = timing.find_field("invocation_count");
        return invocation_count != nullptr && invocation_count->value == "1";
    }));
    auto public_session_diagnostics = runtime.inspect_session();
    auto public_system_diagnostics = runtime.inspect_system_timings();
    assert(public_session_diagnostics && public_system_diagnostics);
    assert(public_session_diagnostics.value().state == "running");
    assert(public_system_diagnostics.value().size() == timing_diagnostics.size());
    assert(runtime.shutdown());

    auto restored_module = std::make_shared<TestGameplayModule>();
    game::RuntimeConfiguration restored_config;
    restored_config.gameplay_modules.push_back(restored_module);
    auto restored_request = make_session_request(report);
    restored_request.initial_snapshot = std::move(save_snapshot).value();
    assert(runtime.start_session(std::move(restored_config), std::move(restored_request)));
    assert(restored_module->persistence_restore_count == 1);
    assert(restored_module->restored_from_snapshot);
    assert(runtime.session()->server()->world().mod_states().find("test", "feature_visible") !=
           nullptr);
    const auto restored_render_snapshot = runtime.capture_render_snapshot();
    assert(restored_render_snapshot && restored_render_snapshot.value().objects.size() == 3);
    assert(runtime.shutdown());
}

void test_replication_tombstone_removes_presentation_proxy() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);
    assert(runtime.start_session({}, make_session_request(report)));
    auto* session = runtime.session();
    auto* server = session->server();
    assert(server != nullptr);
    auto second_client = server->connect_client();
    assert(second_client);
    const auto* second_player = server->player_for_client(second_client.value());
    assert(second_player != nullptr);
    const auto second_player_net_id = second_player->net_id;
    const auto second_player_save_id = second_player->save_id;
    assert(std::ranges::any_of(server->host().replication_relevance_policy().private_access_rules,
                               [second_client, second_player_save_id](const auto& rule) {
                                   return rule.client_id == second_client.value() &&
                                          rule.private_subjects ==
                                              std::vector{second_player_save_id};
                               }));

    movement::PlayerInputFrame input;
    input.tick = 1;
    input.sequence = 1;
    input.move_z = 32'767;
    net::CommandEnvelope command;
    command.sequence = 1;
    command.sender = second_client.value();
    command.type = "player.input";
    command.payload = movement::PlayerInputTextCodec::encode(input);
    command.client_time_ms = 10;
    assert(server->submit_command(second_client.value(), std::move(command)));
    auto appeared = runtime.run_frame({16'667, 17});
    assert(appeared);
    auto snapshot = runtime.capture_render_snapshot();
    assert(snapshot && snapshot.value().objects.size() == 3);
    assert(session->presentation()->find_object(second_player_net_id) != nullptr);

    assert(server->disconnect_client(second_client.value()));
    assert(std::ranges::none_of(
        server->host().replication_relevance_policy().private_access_rules,
        [second_client](const auto& rule) { return rule.client_id == second_client.value(); }));
    auto disappeared = runtime.run_frame({16'667, 34});
    assert(disappeared);
    assert(disappeared.value().server_ticks.front().player_tombstone_count == 1);
    assert(disappeared.value().client.player_tombstone_count == 1);
    assert(disappeared.value().presentation.removed_objects == 1);
    assert(server->events().entity_destroyed.size() == 1);
    snapshot = runtime.capture_render_snapshot();
    assert(snapshot && snapshot.value().objects.size() == 2);
    assert(session->presentation()->find_object(second_player_net_id) == nullptr);
    assert(runtime.shutdown());
}

void test_feature_registries_reject_missing_callbacks() {
    game::PersistenceRegistry persistence;
    auto status = persistence.register_persistence({"test.persistence", 1, {}, {}});
    assert(!status && status.error().code == "gameplay_module.persistence_callback_missing");

    game::ReplicationRegistry replication;
    status = replication.register_replication({"test.replication", 1, true, false, {}});
    assert(!status && status.error().code == "gameplay_module.replication_handler_missing");

    game::PresentationRegistry presentation;
    status = presentation.register_adapter({"test.presentation", 1, {}});
    assert(!status && status.error().code == "gameplay_module.presentation_callback_missing");
}

void test_feature_failures_are_contextual_and_client_failures_are_isolated() {
    const auto report = content::ContentValidation::validate(source_root());
    assert(!report.has_errors());
    auto runtime = make_runtime(report);

    game::RuntimeConfiguration presentation_config;
    presentation_config.gameplay_modules.push_back(std::make_shared<FailingPresentationModule>());
    auto status =
        runtime.start_session(std::move(presentation_config), make_session_request(report));
    assert(!status && status.error().code == "test.presentation_failed");
    assert(status.error().message.find("test.failing_presentation.adapter") != std::string::npos);
    assert(runtime.session() == nullptr);

    game::RuntimeConfiguration persistence_config;
    persistence_config.gameplay_modules.push_back(std::make_shared<FailingPersistenceModule>());
    assert(runtime.start_session(std::move(persistence_config), make_session_request(report)));
    auto snapshot = runtime.capture_save_snapshot();
    assert(!snapshot && snapshot.error().code == "test.persistence_failed");
    assert(snapshot.error().message.find("test.failing_persistence.state") != std::string::npos);
    assert(runtime.session()->is_running());
    assert(runtime.shutdown());

    game::RuntimeConfiguration replication_config;
    replication_config.gameplay_modules.push_back(std::make_shared<FailingReplicationModule>());
    assert(runtime.start_session(std::move(replication_config), make_session_request(report)));
    assert(runtime.submit_command("test.failing_replication.trigger", {}, 10));
    auto frame = runtime.run_frame({16'667, 17});
    assert(frame);
    assert(frame.value().client_presentation_error.has_value());
    assert(frame.value().client_presentation_error->code == "test.replication_failed");
    assert(frame.value().client_presentation_error->message.find(
               "test.failing_replication.delta") != std::string::npos);
    assert(runtime.session()->is_running());
    assert(!runtime.session()->fault().has_value());
    const auto diagnostics = game::GameInspector::inspect(*runtime.session());
    assert(diagnostics.state == "running");
    auto retried = runtime.run_frame({16'667, 34});
    assert(retried);
    assert(!retried.value().client_presentation_error.has_value());
    assert(runtime.submit_command("test.failing_replication.trigger", {}, 20));
    assert(runtime.shutdown());
}

void test_runtime_configuration_rejects_invalid_compositions() {
    game::RuntimeConfiguration empty;
    empty.create_server = false;
    empty.create_client = false;
    assert(!empty.validate());

    game::RuntimeConfiguration loopback_client;
    loopback_client.create_server = false;
    loopback_client.create_client = true;
    loopback_client.use_in_memory_transport = true;
    assert(!loopback_client.validate());

    game::RuntimeConfiguration headless_renderer;
    headless_renderer.create_renderer = true;
    headless_renderer.headless = true;
    assert(!headless_renderer.validate());

    game::RuntimeConfiguration invalid_world_time;
    invalid_world_time.world_time.ticks_per_second = 0;
    assert(!invalid_world_time.validate());
}

} // namespace

int main() {
    test_local_runtime_advances_authority_through_loopback();
    test_client_command_result_history_is_bounded();
    test_selected_scenario_drives_authoritative_bootstrap();
    test_session_rejects_unknown_or_wrong_kind_scenarios();
    test_dedicated_headless_runtime_uses_same_scheduler();
    test_external_listen_runtime_uses_true_remote_endpoint();
    test_two_remote_clients_predict_and_interpolate();
    test_jolt_runtime_moves_on_cooked_terrain();
    test_collision_revision_preserves_pending_look_and_position();
    test_boundary_voxel_edit_rebuilds_collision_and_removes_support();
    test_jolt_runtime_drops_settles_and_restores_physical_resource();
    test_authoritative_player_input_moves_and_replicates();
    test_transient_replication_budget_defers_latest_state();
    test_prediction_under_deterministic_100ms_rtt_and_two_percent_loss();
    test_typed_voxel_commands_validate_and_replicate();
    test_runtime_relights_and_replicates_chunk_light();
    test_runtime_simulates_and_replicates_voxel_fluid();
    test_session_save_and_reload_restores_authoritative_state();
    test_boundary_voxel_edits_survive_restart();
    test_foundation_save_rejects_incompatible_layout();
    test_session_file_load_preserves_missing_prototypes();
    test_session_load_restores_persisted_missing_voxel_palette();
    test_gameplay_modules_extend_runtime_through_registration_contract();
    test_replication_tombstone_removes_presentation_proxy();
    test_feature_registries_reject_missing_callbacks();
    test_feature_failures_are_contextual_and_client_failures_are_isolated();
    test_runtime_configuration_rejects_invalid_compositions();
    return 0;
}
