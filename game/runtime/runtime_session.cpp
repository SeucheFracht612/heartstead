#include "game/runtime/runtime_session.hpp"

#include "engine/core/hash.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/profiling/profiler.hpp"
#include "engine/scenarios/scenario_fixture.hpp"
#include "engine/scenarios/scenario_prototype.hpp"
#include "engine/world/world_snapshot.hpp"
#include "engine/world/worldgen/terrain_generator.hpp"
#include "game/foundation/foundation_world.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] core::Status startup_cancelled(std::stop_token stop_token) {
    if (!stop_token.stop_requested()) {
        return core::Status::ok();
    }
    return core::Status::failure("session_startup.cancelled", "session startup was cancelled");
}

constexpr std::string_view runtime_tick_state_mod = "engine";
constexpr std::string_view runtime_tick_state_key = "runtime.fixed_step_tick";
constexpr std::string_view generator_state_key = "world.generator_preset";
constexpr std::string_view generator_version_state_key = "world.generator_version";
constexpr std::uint32_t renderer_proof_local_outbound_budget = 16U * 1024U * 1024U;
constexpr std::size_t renderer_proof_local_chunks_per_frame = 2;

[[nodiscard]] core::Result<std::uint64_t>
saved_fixed_step_tick(const save::SaveSnapshot& snapshot) {
    const auto found = std::ranges::find_if(snapshot.mod_states, [](const auto& state) {
        return state.mod_id == runtime_tick_state_mod && state.state_key == runtime_tick_state_key;
    });
    if (found == snapshot.mod_states.end()) {
        return core::Result<std::uint64_t>::success(0);
    }
    std::uint64_t tick = 0;
    const auto [end, error] =
        std::from_chars(found->encoded_state.data(),
                        found->encoded_state.data() + found->encoded_state.size(), tick);
    if (error != std::errc{} || end != found->encoded_state.data() + found->encoded_state.size()) {
        return core::Result<std::uint64_t>::failure(
            "runtime_session.invalid_saved_tick",
            "saved fixed-step tick is not an unsigned integer");
    }
    return core::Result<std::uint64_t>::success(tick);
}

[[nodiscard]] std::string content_session_fingerprint(const save::SaveMetadata& metadata) {
    core::StableHash64 hash;
    hash.add_string(metadata.game_version);
    hash.add_u64_le(metadata.schema_version);
    for (const auto& mod : metadata.enabled_mods) {
        hash.add_string(mod.id);
        hash.add_byte(0);
        hash.add_string(mod.version);
        hash.add_byte(0);
        hash.add_string(mod.prototype_hash);
        hash.add_byte(0xffU);
    }
    return "content-" + hash.hex();
}

[[nodiscard]] core::Status validate_foundation_layout(const save::SaveSnapshot& snapshot,
                                                      const core::PrototypeId& scenario_id) {
    if (scenario_id.value() != foundation::scenario_id) {
        return core::Status::ok();
    }
    const auto saved_layout =
        std::ranges::find_if(snapshot.mod_states, [](const save::ModStateSaveRecord& record) {
            return record.mod_id == foundation::layout_state_mod &&
                   record.state_key == foundation::layout_state_key;
        });
    if (saved_layout == snapshot.mod_states.end()) {
        return core::Status::failure("foundation_world.layout_version_missing",
                                     "Foundation save does not record the baseline layout version");
    }
    std::uint32_t version = 0;
    const auto [end, error] = std::from_chars(
        saved_layout->encoded_state.data(),
        saved_layout->encoded_state.data() + saved_layout->encoded_state.size(), version);
    if (error != std::errc{} ||
        end != saved_layout->encoded_state.data() + saved_layout->encoded_state.size()) {
        return core::Status::failure(
            "foundation_world.layout_version_invalid",
            "Foundation save contains an invalid baseline layout version: " +
                saved_layout->encoded_state);
    }
    if (version != foundation::layout_version) {
        return core::Status::failure("foundation_world.layout_version_mismatch",
                                     "Foundation save layout version " + std::to_string(version) +
                                         " is incompatible with current layout version " +
                                         std::to_string(foundation::layout_version));
    }
    return core::Status::ok();
}

} // namespace

core::Status RuntimeConfiguration::validate() const {
    auto status = fixed_step.validate();
    if (!status) {
        return status;
    }
    status = world_time.validate();
    if (!status) {
        return status;
    }
    status = chunk_lighting.validate();
    if (!status) {
        return status;
    }
    status = chunk_fluids.validate();
    if (!status) {
        return status;
    }
    if (max_transient_snapshot_messages_per_tick == 0 ||
        max_transient_snapshot_payload_bytes_per_tick == 0 ||
        max_outbound_bytes_per_client_per_second == 0) {
        return core::Status::failure(
            "runtime_configuration.invalid_replication_budget",
            "transient replication and per-client outbound budgets must be non-zero");
    }
    if (simulated_network_one_way_latency_ms > 60'000 || simulated_network_jitter_ms > 60'000 ||
        simulated_network_unreliable_loss_basis_points > 10'000) {
        return core::Status::failure(
            "runtime_configuration.invalid_network_impairment",
            "simulated latency/jitter must be at most 60 seconds and loss at most 100 percent");
    }
    if (!use_in_memory_transport &&
        (simulated_network_one_way_latency_ms != 0 || simulated_network_jitter_ms != 0 ||
         simulated_network_unreliable_loss_basis_points != 0)) {
        return core::Status::failure(
            "runtime_configuration.external_impairment_unsupported",
            "deterministic in-process impairment is available only on the in-memory transport; "
            "use tc netem for POSIX UDP");
    }
    if (!create_server && !create_client) {
        return core::Status::failure("runtime_configuration.empty",
                                     "runtime must create a server, client, or both");
    }
    if (create_client && !create_server && use_in_memory_transport) {
        return core::Status::failure(
            "runtime_configuration.loopback_without_server",
            "an in-memory client requires an authoritative server in the same process");
    }
    if (!use_in_memory_transport) {
        if (create_server) {
            status = net::validate_transport_endpoint(server_bind_endpoint);
            if (!status) {
                return status;
            }
        }
        if (create_client && !create_server && !remote_server_endpoint.has_value()) {
            return core::Status::failure("runtime_configuration.remote_endpoint_missing",
                                         "remote-client-only runtime requires a server endpoint");
        }
        if (remote_server_endpoint.has_value()) {
            status = net::validate_transport_endpoint(*remote_server_endpoint);
            if (!status || remote_server_endpoint->port == 0) {
                return !status
                           ? status
                           : core::Status::failure("runtime_configuration.remote_endpoint_port",
                                                   "remote server endpoint port must be non-zero");
            }
        }
    }
    return core::Status::ok();
}

core::Status SessionLaunchRequest::validate() const {
    if (initial_runtime_time_ms < 0) {
        return core::Status::failure("session_launch.invalid_initial_time",
                                     "initial runtime time must be nonnegative");
    }
    if (scenario_id.empty()) {
        return core::Status::failure("session_launch.invalid_scenario",
                                     "session scenario id must not be empty");
    }
    if (!metadata.validate()) {
        return core::Status::failure("session_launch.invalid_metadata",
                                     "session save metadata is invalid");
    }
    if (seed.has_value() && metadata.world_seed != 0 && *seed != metadata.world_seed) {
        return core::Status::failure("session_launch.seed_mismatch",
                                     "requested seed does not match prepared world metadata");
    }
    if (player_spawn.has_value() &&
        (!player_spawn->position.is_valid() || !std::isfinite(player_spawn->yaw_degrees) ||
         !std::isfinite(player_spawn->pitch_degrees))) {
        return core::Status::failure("session_launch.invalid_spawn",
                                     "player spawn position and orientation must be finite");
    }
    if (mode == SessionMode::remote_multiplayer && !network_endpoint.has_value()) {
        return core::Status::failure("session_launch.remote_endpoint_missing",
                                     "remote multiplayer requires a network endpoint");
    }
    if (world_source == WorldSourceKind::existing_save && !save_path.has_value() &&
        !initial_snapshot.has_value()) {
        return core::Status::failure(
            "session_launch.save_source_missing",
            "existing-save launch requires a save path or loaded snapshot");
    }
    if (mode == SessionMode::replay || world_source == WorldSourceKind::replay) {
        return core::Status::failure("session_launch.replay_unsupported",
                                     "this build has no replay runtime");
    }
    return runtime.validate();
}

RuntimeSession::RuntimeSession(RuntimeConfiguration config, SessionRequest request,
                               const modding::PrototypeRegistry& prototypes,
                               const world::VoxelPalette& voxel_palette)
    : config_(std::move(config)), request_(std::move(request)), prototypes_(&prototypes),
      voxel_palette_(&voxel_palette), fixed_step_(config_.fixed_step),
      last_tick_time_ms_(request_.initial_runtime_time_ms) {}

RuntimeSession::~RuntimeSession() {
    (void)shutdown();
}

core::Result<std::unique_ptr<RuntimeSession>>
RuntimeSession::create(RuntimeConfiguration config, SessionRequest request,
                       const modding::PrototypeRegistry& prototypes,
                       const world::VoxelPalette& voxel_palette) {
    return create(std::move(config), std::move(request), prototypes, voxel_palette, {}, {});
}

core::Result<std::unique_ptr<RuntimeSession>>
RuntimeSession::create(RuntimeConfiguration config, SessionRequest request,
                       const modding::PrototypeRegistry& prototypes,
                       const world::VoxelPalette& voxel_palette,
                       SessionStartupProgressCallback progress, std::stop_token stop_token) {
    if (progress) {
        progress(SessionStartupPhase::validating_request);
    }
    auto cancellation = startup_cancelled(stop_token);
    if (!cancellation) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(cancellation.error().code,
                                                                      cancellation.error().message);
    }
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(status.error().code,
                                                                      status.error().message);
    }
    if (request.initial_snapshot.has_value()) {
        request.metadata = request.initial_snapshot->metadata;
    }
    request.runtime = config;
    status = request.validate();
    if (!status) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(status.error().code,
                                                                      status.error().message);
    }
    auto session = std::unique_ptr<RuntimeSession>(
        new RuntimeSession(std::move(config), std::move(request), prototypes, voxel_palette));
    status = session->initialize(progress, stop_token);
    if (!status) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(status.error().code,
                                                                      status.error().message);
    }
    return core::Result<std::unique_ptr<RuntimeSession>>::success(std::move(session));
}

core::Status RuntimeSession::initialize(const SessionStartupProgressCallback& progress,
                                        std::stop_token stop_token) {
    state_ = RuntimeSessionState::starting;
    auto cancellation = startup_cancelled(stop_token);
    if (!cancellation) {
        return cancellation;
    }
    if (request_.initial_snapshot.has_value()) {
        if (progress) {
            progress(SessionStartupPhase::restoring_world);
        }
        auto saved_tick = saved_fixed_step_tick(*request_.initial_snapshot);
        if (!saved_tick) {
            return core::Status::failure(saved_tick.error().code, saved_tick.error().message);
        }
        fixed_step_.reset(saved_tick.value());
    }
    if (progress) {
        progress(SessionStartupPhase::preparing_world);
    }
    const auto scenario_id = core::PrototypeId::parse(request_.scenario_id);
    if (!scenario_id) {
        return core::Status::failure("runtime_session.invalid_scenario_id",
                                     "session scenario id is not a valid prototype id: " +
                                         request_.scenario_id);
    }
    const auto* scenario_prototype = prototypes_->find(scenario_id.value());
    if (scenario_prototype == nullptr) {
        return core::Status::failure("runtime_session.scenario_missing",
                                     "session scenario prototype is not loaded: " +
                                         request_.scenario_id);
    }
    auto scenario = scenarios::scenario_definition_from_prototype(*scenario_prototype);
    if (!scenario) {
        return core::Status::failure(scenario.error().code, scenario.error().message);
    }
    if (request_.player_spawn.has_value()) {
        scenario.value().spawn_position = request_.player_spawn->position;
        scenario.value().spawn_yaw_degrees = request_.player_spawn->yaw_degrees;
        scenario.value().spawn_pitch_degrees = request_.player_spawn->pitch_degrees;
    }
    if (request_.initial_snapshot.has_value()) {
        const auto saved_scenario = std::ranges::find_if(
            request_.initial_snapshot->mod_states, [](const save::ModStateSaveRecord& record) {
                return record.mod_id == "engine" && record.state_key == "scenario.id";
            });
        if (saved_scenario != request_.initial_snapshot->mod_states.end() &&
            saved_scenario->encoded_state != scenario.value().prototype_id.value()) {
            return core::Status::failure(
                "runtime_session.scenario_mismatch",
                "requested scenario does not match the scenario recorded by the save");
        }
        auto layout_status =
            validate_foundation_layout(*request_.initial_snapshot, scenario.value().prototype_id);
        if (!layout_status) {
            return layout_status;
        }
    }

    if (config_.create_server) {
        if (progress) {
            progress(SessionStartupPhase::starting_authoritative_server);
        }
        ServerRuntimeDesc server_desc;
        server_desc.world.metadata = request_.metadata;
        server_desc.world.voxel_palette = voxel_palette_->manifest();
        server_desc.host.transport.backend = config_.use_in_memory_transport
                                                 ? net::TransportBackend::in_memory
                                                 : net::TransportBackend::external_library;
        server_desc.host.transport.external.bind_endpoint = config_.server_bind_endpoint;
        server_desc.host.transport.external.content_fingerprint =
            content_session_fingerprint(request_.metadata);
        server_desc.host.transport.in_memory.simulated_one_way_latency_ms =
            config_.simulated_network_one_way_latency_ms;
        server_desc.host.transport.in_memory.simulated_jitter_ms =
            config_.simulated_network_jitter_ms;
        server_desc.host.transport.in_memory.simulated_unreliable_loss_basis_points =
            config_.simulated_network_unreliable_loss_basis_points;
        server_desc.host.transport.in_memory.impairment_seed = config_.simulated_network_seed;
        server_desc.host.max_outbound_bytes_per_client_per_second =
            config_.max_outbound_bytes_per_client_per_second;
        if (config_.use_in_memory_transport && scenario.value().setup_hook == "renderer_proof") {
            local_renderer_proof_chunk_fast_path_ = true;
            server_desc.direct_local_chunk_replication = true;
            server_desc.host.max_outbound_bytes_per_client_per_second =
                std::max(server_desc.host.max_outbound_bytes_per_client_per_second,
                         renderer_proof_local_outbound_budget);
        }
        server_desc.physics.backend = config_.physics_backend;
        // Physics remains in a bounded float island. Anchor that island at the launch spawn so
        // packaged worlds and saves at very large coordinates never pass absolute coordinates to
        // the physics backend.
        server_desc.chunk_collision.physics_island.block =
            scenario.value().spawn_position.has_value() ? scenario.value().spawn_position->anchor
                                                        : world::BlockCoord{};
        server_desc.chunk_fluids = config_.chunk_fluids;
        server_desc.chunk_lighting = config_.chunk_lighting;
        server_desc.simulation_ticks_per_second = config_.fixed_step.ticks_per_second;
        server_desc.max_transient_snapshot_messages_per_tick =
            config_.max_transient_snapshot_messages_per_tick;
        server_desc.max_transient_snapshot_payload_bytes_per_tick =
            config_.max_transient_snapshot_payload_bytes_per_tick;
        server_desc.world_time = config_.world_time;
        server_desc.prototypes = prototypes_;
        server_desc.voxel_palette = voxel_palette_;
        server_desc.scenario = std::move(scenario).value();
        server_desc.initial_snapshot = request_.initial_snapshot;
        server_desc.gameplay_modules = config_.gameplay_modules;
        server_desc.stop_token = stop_token;
        server_desc.startup_progress = [progress](ServerRuntimeStartupPhase phase) {
            if (!progress) {
                return;
            }
            switch (phase) {
            case ServerRuntimeStartupPhase::restoring_world:
                progress(SessionStartupPhase::restoring_world);
                break;
            case ServerRuntimeStartupPhase::initializing_physics:
                progress(SessionStartupPhase::initializing_physics);
                break;
            case ServerRuntimeStartupPhase::generating_spawn_area:
                progress(SessionStartupPhase::generating_spawn_area);
                break;
            case ServerRuntimeStartupPhase::registering_gameplay_systems:
                progress(SessionStartupPhase::registering_gameplay_systems);
                break;
            }
        };
        cancellation = startup_cancelled(stop_token);
        if (!cancellation) {
            return cancellation;
        }
        auto server = ServerRuntime::create(std::move(server_desc));
        if (!server) {
            return core::Status::failure(server.error().code, server.error().message);
        }
        server_ = std::move(server).value();
        cancellation = startup_cancelled(stop_token);
        if (!cancellation) {
            return cancellation;
        }
        auto status = server_->start();
        if (!status) {
            return status;
        }
    }

    if (config_.create_client) {
        cancellation = startup_cancelled(stop_token);
        if (!cancellation) {
            return cancellation;
        }
        if (progress) {
            progress(SessionStartupPhase::starting_client);
        }
        world::WorldStateDesc client_world;
        client_world.metadata = request_.metadata;
        client_world.voxel_palette = voxel_palette_->manifest();
        if (progress) {
            progress(SessionStartupPhase::connecting_transport);
        }
        if (config_.use_in_memory_transport) {
            if (server_ == nullptr) {
                return core::Status::failure(
                    "runtime_session.local_server_missing",
                    "in-memory client requires a local authoritative server");
            }
            auto connected = server_->connect_client();
            if (!connected) {
                return core::Status::failure(connected.error().code, connected.error().message);
            }
            client_ =
                std::make_unique<ClientRuntime>(connected.value(), std::move(client_world),
                                                &server_->replication_registry(), voxel_palette_);
        } else {
            auto endpoint = config_.remote_server_endpoint;
            if (!endpoint.has_value() && server_ != nullptr) {
                endpoint = server_->host().local_endpoint();
                if (endpoint.has_value() &&
                    (endpoint->address == "0.0.0.0" || endpoint->address.empty())) {
                    endpoint->address = "127.0.0.1";
                }
            }
            if (!endpoint.has_value() || endpoint->port == 0) {
                return core::Status::failure(
                    "runtime_session.remote_endpoint_unavailable",
                    "remote client could not resolve the server bind endpoint");
            }
            net::ExternalTransportClientConfig transport_config;
            transport_config.server_endpoint = *endpoint;
            transport_config.content_fingerprint = content_session_fingerprint(request_.metadata);
            auto remote = net::create_external_transport_client(std::move(transport_config));
            if (!remote) {
                return core::Status::failure(remote.error().code, remote.error().message);
            }
            remote_transport_ = std::move(remote).value();
            cancellation = startup_cancelled(stop_token);
            if (!cancellation) {
                return cancellation;
            }
            auto status = remote_transport_->connect(request_.initial_runtime_time_ms);
            if (!status) {
                return status;
            }
            client_ = std::make_unique<ClientRuntime>(
                core::NetId{}, std::move(client_world),
                server_ == nullptr ? nullptr : &server_->replication_registry(), voxel_palette_);
        }
        auto status = pump_client_messages(request_.initial_runtime_time_ms);
        if (!status) {
            return status;
        }
        const auto impaired_in_memory = config_.simulated_network_one_way_latency_ms != 0 ||
                                        config_.simulated_network_jitter_ms != 0 ||
                                        config_.simulated_network_unreliable_loss_basis_points != 0;
        if (config_.use_in_memory_transport && !impaired_in_memory && !client_->is_connected()) {
            return core::Status::failure("runtime_session.client_handshake_failed",
                                         "local client did not accept the server welcome");
        }
        auto synchronized = client_->synchronize(0, std::numeric_limits<std::size_t>::max());
        if (!synchronized) {
            return core::Status::failure(synchronized.error().code, synchronized.error().message);
        }
        if (progress) {
            progress(SessionStartupPhase::constructing_presentation);
        }
        cancellation = startup_cancelled(stop_token);
        if (!cancellation) {
            return cancellation;
        }
        auto presented = synchronize_presentation();
        if (!presented) {
            return core::Status::failure(presented.error().code, presented.error().message);
        }
    }
    state_ = RuntimeSessionState::running;
    accepting_commands_ = true;
    if (progress && connection_state() != SessionConnectionState::connecting) {
        progress(SessionStartupPhase::ready);
    }
    return core::Status::ok();
}

core::Result<RuntimeFrameStats> RuntimeSession::run_frame(RuntimeFrameInput input) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("runtime.frame");
    if (fault_.has_value()) {
        return core::Result<RuntimeFrameStats>::failure("runtime_session.faulted",
                                                        "runtime session cannot continue after '" +
                                                            fault_->code + "': " + fault_->message);
    }
    if (state_ != RuntimeSessionState::running) {
        return core::Result<RuntimeFrameStats>::failure(
            "runtime_session.not_running", "runtime session must be running before frame advance");
    }
    auto frame = fixed_step_.advance(input.frame_time_us);
    if (!frame) {
        return core::Result<RuntimeFrameStats>::failure(frame.error().code, frame.error().message);
    }

    RuntimeFrameStats stats;
    stats.fixed_step = frame.value();
    if (server_ != nullptr) {
        stats.authoritative_world_tick = server_->world().world_time();
    }
    const auto update_client = [this, &stats, &input](std::uint64_t render_tick) {
        auto status = pump_client_messages(input.now_ms);
        if (!status) {
            return status;
        }
        if (client_ == nullptr || !client_->is_connected()) {
            return core::Status::ok();
        }
        auto synchronized = client_->synchronize(render_tick);
        if (!synchronized) {
            return core::Status::failure(synchronized.error().code, synchronized.error().message);
        }
        stats.client = std::move(synchronized).value();
        auto presented = synchronize_presentation();
        if (!presented) {
            return core::Status::failure(presented.error().code, presented.error().message);
        }
        stats.presentation = std::move(presented).value();
        return core::Status::ok();
    };
    stats.server_ticks.reserve(frame.value().step_count);
    for (std::uint32_t step = 0; step < frame.value().step_count; ++step) {
        if (server_ != nullptr) {
            const auto tick = frame.value().first_tick + step;
            const auto remaining_steps = frame.value().step_count - step - 1;
            const auto remaining_ms = static_cast<std::int64_t>(
                (static_cast<std::uint64_t>(remaining_steps) * frame.value().step_us) / 1'000U);
            const auto tick_time_ms = std::max(last_tick_time_ms_ + 1, input.now_ms - remaining_ms);
            auto tick_result = server_->run_tick(
                tick, 1.0 / static_cast<double>(config_.fixed_step.ticks_per_second), tick_time_ms);
            if (!tick_result) {
                return fault_frame(tick_result.error());
            }
            stats.server_ticks.push_back(std::move(tick_result).value());
            stats.authoritative_world_tick = server_->world().world_time();
            last_tick_time_ms_ = tick_time_ms;
        }
    }
    auto local_chunk_status = synchronize_local_renderer_proof_chunks();
    if (!local_chunk_status) {
        return fault_frame(local_chunk_status.error());
    }
    const auto render_tick = server_ == nullptr ? 0 : fixed_step_.tick();
    auto client_status = update_client(render_tick);
    if (!client_status) {
        if (server_ == nullptr) {
            return fault_frame(client_status.error());
        }
        stats.client_presentation_error = client_status.error();
        presentation_synchronizer_.clear();
        presentation_.clear();
    }
    last_frame_stats_ = stats;
    ++frame_count_;
    return core::Result<RuntimeFrameStats>::success(std::move(stats));
}

core::Result<RuntimeFrameStats> RuntimeSession::fault_frame(const core::Error& error) {
    fault_ = error;
    accepting_commands_ = false;
    state_ = RuntimeSessionState::faulted;
    return core::Result<RuntimeFrameStats>::failure(error.code, error.message);
}

core::Status RuntimeSession::submit_command(std::string type, std::string payload,
                                            std::int64_t now_ms) {
    auto submitted = submit_tracked_command(std::move(type), std::move(payload), now_ms);
    if (!submitted) {
        return core::Status::failure(submitted.error().code, submitted.error().message);
    }
    return core::Status::ok();
}

core::Result<std::uint64_t>
RuntimeSession::submit_tracked_command(std::string type, std::string payload, std::int64_t now_ms) {
    if (!accepting_commands_ || state_ != RuntimeSessionState::running || client_ == nullptr ||
        (server_ == nullptr && remote_transport_ == nullptr)) {
        return core::Result<std::uint64_t>::failure(
            "runtime_session.command_path_unavailable",
            "commands require an active client and authoritative server connection");
    }
    auto command = client_->create_command(std::move(type), std::move(payload), now_ms);
    if (!command) {
        return core::Result<std::uint64_t>::failure(command.error().code, command.error().message);
    }
    const auto sequence = command.value().sequence;
    auto status = remote_transport_ != nullptr
                      ? remote_transport_->send_to_server(
                            net::make_command_transport_message(command.value()))
                      : server_->submit_command(client_->client_id(), std::move(command).value());
    if (!status) {
        return core::Result<std::uint64_t>::failure(status.error().code, status.error().message);
    }
    return core::Result<std::uint64_t>::success(sequence);
}

core::Result<std::uint64_t>
RuntimeSession::submit_inventory_transfer(const world::InventoryTransferRequest& request,
                                          std::int64_t now_ms) {
    if (!request.source_owner_id.is_valid() || !request.destination_owner_id.is_valid() ||
        request.count == 0) {
        return core::Result<std::uint64_t>::failure(
            "runtime_session.invalid_inventory_transfer",
            "inventory transfer requires valid owners and a non-zero count");
    }
    net::CommandPayload payload;
    const std::array fields{
        std::pair{"source_owner", request.source_owner_id.to_string()},
        std::pair{"destination_owner", request.destination_owner_id.to_string()},
        std::pair{"source_slot", std::to_string(request.source_slot)},
        std::pair{"destination_slot", std::to_string(request.destination_slot)},
        std::pair{"count", std::to_string(request.count)},
    };
    for (const auto& [key, value] : fields) {
        auto status = payload.set(key, value);
        if (!status) {
            return core::Result<std::uint64_t>::failure(status.error().code,
                                                        status.error().message);
        }
    }
    return submit_tracked_command("inventory.transfer_items",
                                  net::CommandPayloadTextCodec::encode(payload), now_ms);
}

core::Status RuntimeSession::submit_player_input(const movement::PlayerInputFrame& input,
                                                 std::int64_t now_ms) {
    auto network_input = input;
    if (client_ != nullptr) {
        const auto* predicted = client_->local_player_snapshot();
        if (predicted != nullptr && network_input.tick <= predicted->state.simulation_tick) {
            if (predicted->state.simulation_tick == std::numeric_limits<std::uint64_t>::max()) {
                return core::Status::failure(
                    "runtime_session.movement_tick_exhausted",
                    "local movement prediction exhausted its 64-bit tick space");
            }
            network_input.tick = predicted->state.simulation_tick + 1;
        }
    }
    auto status = network_input.validate();
    if (!status) {
        return status;
    }
    if (!accepting_commands_ || state_ != RuntimeSessionState::running || client_ == nullptr ||
        (server_ == nullptr && remote_transport_ == nullptr) || !client_->is_connected()) {
        return core::Status::failure("runtime_session.movement_path_unavailable",
                                     "movement input requires an active connected client");
    }
    auto bundle = client_->movement_input_bundle(network_input);
    if (!bundle) {
        return core::Status::failure(bundle.error().code, bundle.error().message);
    }
    status = remote_transport_ != nullptr
                 ? remote_transport_->send_to_server(
                       movement::make_movement_input_bundle_message(bundle.value(), now_ms))
                 : server_->submit_movement_input(client_->client_id(), bundle.value(), now_ms);
    if (!status) {
        return status;
    }
    return client_->predict_local_input(bundle.value().frames.back());
}

core::Status RuntimeSession::submit_place_voxel(const interaction::PlaceVoxelCommand& command,
                                                std::int64_t now_ms) {
    return submit_command(std::string(interaction::place_voxel_command_type),
                          interaction::VoxelCommandTextCodec::encode(command), now_ms);
}

core::Status RuntimeSession::submit_remove_voxel(const interaction::RemoveVoxelCommand& command,
                                                 std::int64_t now_ms) {
    return submit_command(std::string(interaction::remove_voxel_command_type),
                          interaction::VoxelCommandTextCodec::encode(command), now_ms);
}

core::Result<save::SaveSnapshot> RuntimeSession::capture_save_snapshot() const {
    if (state_ != RuntimeSessionState::running || server_ == nullptr) {
        return core::Result<save::SaveSnapshot>::failure(
            "runtime_session.no_authoritative_world",
            "saving requires an active authoritative server runtime");
    }
    auto snapshot = world::WorldSnapshotBridge::export_snapshot(server_->world());
    if (!snapshot) {
        return snapshot;
    }
    auto status = server_->persistence_registry().capture_all(server_->world(), snapshot.value());
    if (!status) {
        return core::Result<save::SaveSnapshot>::failure(status.error().code,
                                                         status.error().message);
    }
    auto runtime_tick_state =
        std::ranges::find_if(snapshot.value().mod_states, [](const auto& state) {
            return state.mod_id == runtime_tick_state_mod &&
                   state.state_key == runtime_tick_state_key;
        });
    if (runtime_tick_state == snapshot.value().mod_states.end()) {
        snapshot.value().mod_states.push_back({std::string(runtime_tick_state_mod),
                                               std::string(runtime_tick_state_key),
                                               std::to_string(fixed_step_.tick())});
    } else {
        runtime_tick_state->encoded_state = std::to_string(fixed_step_.tick());
    }
    auto generator_state = std::ranges::find_if(snapshot.value().mod_states, [](const auto& state) {
        return state.mod_id == runtime_tick_state_mod && state.state_key == generator_state_key;
    });
    if (generator_state == snapshot.value().mod_states.end()) {
        snapshot.value().mod_states.push_back(
            {std::string(runtime_tick_state_mod), std::string(generator_state_key),
             request_.generator_preset.empty() ? "unknown" : request_.generator_preset});
    } else {
        generator_state->encoded_state =
            request_.generator_preset.empty() ? "unknown" : request_.generator_preset;
    }
    auto generator_version_state =
        std::ranges::find_if(snapshot.value().mod_states, [](const auto& state) {
            return state.mod_id == runtime_tick_state_mod &&
                   state.state_key == generator_version_state_key;
        });
    const auto generator_version = std::to_string(world::deterministic_terrain_generator_version);
    if (generator_version_state == snapshot.value().mod_states.end()) {
        snapshot.value().mod_states.push_back({std::string(runtime_tick_state_mod),
                                               std::string(generator_version_state_key),
                                               generator_version});
    } else {
        generator_version_state->encoded_state = generator_version;
    }
    for (const auto* player : server_->players().records()) {
        if (!player->persistent) {
            continue;
        }
        const auto entity = std::ranges::find(snapshot.value().entities, player->save_id,
                                              &save::EntitySaveRecord::save_id);
        if (entity == snapshot.value().entities.end()) {
            continue;
        }
        movement::PlayerControllerSnapshot controller;
        controller.player_net_id = player->net_id;
        controller.player_save_id = player->save_id;
        controller.state = player->state;
        controller.last_processed_input_sequence = player->state.last_input_sequence;
        controller.collision_world_revision = 0;
        entity->encoded_state = std::string(movement::player_controller_save_state_magic) +
                                movement::PlayerControllerSnapshotTextCodec::encode(controller);
    }
    const auto validation = save::SaveSnapshotValidator::validate(snapshot.value(), *prototypes_);
    if (!validation.valid()) {
        return core::Result<save::SaveSnapshot>::failure(
            "runtime_session.feature_snapshot_invalid",
            validation.issues.empty() ? "feature persistence produced an invalid save snapshot"
                                      : validation.issues.front().message);
    }
    return snapshot;
}

core::Status RuntimeSession::save_to(const save::FileSaveDatabase& database) const {
    auto snapshot = capture_save_snapshot();
    if (!snapshot) {
        return core::Status::failure(snapshot.error().code, snapshot.error().message);
    }
    return database.write_snapshot(snapshot.value());
}

RenderSnapshot RuntimeSession::capture_render_snapshot() const {
    const auto tick = server_ == nullptr && client_ != nullptr
                          ? client_->latest_authoritative_tick()
                          : fixed_step_.tick();
    return presentation_.extract(tick);
}

core::Status RuntimeSession::pump_client_messages(std::int64_t now_ms) {
    if (client_ == nullptr) {
        return core::Status::ok();
    }
    if (remote_transport_ != nullptr) {
        auto maintenance = remote_transport_->poll_maintenance(now_ms);
        if (!maintenance) {
            return core::Status::failure(maintenance.error().code, maintenance.error().message);
        }
        if (maintenance.value().disconnected) {
            const auto reason = maintenance.value().disconnect_reason_code.empty()
                                    ? std::string("transport_client.disconnected")
                                    : maintenance.value().disconnect_reason_code;
            client_->session().mark_transport_disconnected(reason, "remote transport disconnected",
                                                           now_ms);
            return core::Status::failure(reason, "remote transport disconnected");
        }
        const auto messages = remote_transport_->drain_server_messages();
        auto status = client_->receive(messages);
        if (!status || client_->is_connected() ||
            remote_transport_->state() != net::TransportClientState::disconnected) {
            return status;
        }
        const auto session_stats = client_->session().stats();
        return core::Status::failure(session_stats.disconnect_reason_code.empty()
                                         ? "transport_client.disconnected"
                                         : session_stats.disconnect_reason_code,
                                     session_stats.disconnect_reason_message.empty()
                                         ? "remote server disconnected the client"
                                         : session_stats.disconnect_reason_message);
    }
    if (server_ == nullptr) {
        return core::Status::ok();
    }
    auto messages = server_->drain_client_messages(client_->client_id());
    return !messages ? core::Status::failure(messages.error().code, messages.error().message)
                     : client_->receive(messages.value());
}

core::Status RuntimeSession::synchronize_local_renderer_proof_chunks() {
    if (!local_renderer_proof_chunk_fast_path_ || server_ == nullptr || client_ == nullptr) {
        return core::Status::ok();
    }
    std::vector<const world::VoxelChunk*> missing;
    for (const auto* chunk : server_->world().chunks().records()) {
        if (client_->world().chunks().find(chunk->coord()) == nullptr) {
            missing.push_back(chunk);
        }
    }
    std::ranges::sort(missing, [](const auto* lhs, const auto* rhs) {
        const auto distance_squared = [](world::ChunkCoord coord) {
            const auto x = coord.x - scenarios::renderer_proof_center.x;
            const auto z = coord.z - scenarios::renderer_proof_center.z;
            return x * x + z * z;
        };
        const auto lhs_distance = distance_squared(lhs->coord());
        const auto rhs_distance = distance_squared(rhs->coord());
        return lhs_distance != rhs_distance ? lhs_distance < rhs_distance
                                            : lhs->coord() < rhs->coord();
    });
    const auto count = std::min(renderer_proof_local_chunks_per_frame, missing.size());
    for (std::size_t index = 0; index < count; ++index) {
        auto status = client_->install_local_chunk_snapshot(*missing[index]);
        if (!status) {
            return status;
        }
    }
    return core::Status::ok();
}

core::Result<PresentationSynchronizationStats> RuntimeSession::synchronize_presentation() {
    if (client_ == nullptr) {
        return core::Result<PresentationSynchronizationStats>::success({});
    }
    auto synchronized = presentation_synchronizer_.synchronize(*client_, presentation_);
    if (!synchronized) {
        return synchronized;
    }
    if (server_ == nullptr) {
        return synchronized;
    }
    auto feature_adapters =
        server_->presentation_registry().synchronize_all(*client_, presentation_);
    if (!feature_adapters) {
        return core::Result<PresentationSynchronizationStats>::failure(
            feature_adapters.error().code, feature_adapters.error().message);
    }
    synchronized.value().merge(feature_adapters.value());
    return synchronized;
}

core::Status RuntimeSession::request_stop() {
    accepting_commands_ = false;
    teardown_report_.rejected_new_commands = true;
    if (state_ == RuntimeSessionState::running || state_ == RuntimeSessionState::faulted ||
        state_ == RuntimeSessionState::starting) {
        state_ = RuntimeSessionState::stopping;
    }
    return core::Status::ok();
}

core::Status RuntimeSession::register_cleanup(std::string name,
                                              std::function<core::Status()> cleanup) {
    if (state_ != RuntimeSessionState::running || !accepting_commands_) {
        return core::Status::failure("runtime_session.cleanup_registration_closed",
                                     "session cleanup can only be registered while running");
    }
    if (name.empty() || !cleanup) {
        return core::Status::failure("runtime_session.invalid_cleanup",
                                     "session cleanup requires a name and callback");
    }
    if (std::ranges::any_of(cleanup_entries_,
                            [&name](const CleanupEntry& entry) { return entry.name == name; })) {
        return core::Status::failure("runtime_session.duplicate_cleanup",
                                     "session cleanup callback is already registered: " + name);
    }
    cleanup_entries_.push_back({std::move(name), std::move(cleanup)});
    return core::Status::ok();
}

core::Status RuntimeSession::shutdown() {
    ++teardown_report_.invocation_count;
    teardown_report_.ownership_generation = request_.ownership_generation;
    if (state_ == RuntimeSessionState::stopped && server_ == nullptr && client_ == nullptr &&
        remote_transport_ == nullptr && cleanup_entries_.empty()) {
        return core::Status::ok();
    }
    teardown_report_.presentation_objects_before = presentation_.stats().retained_object_count;
    teardown_report_.server_entities_before =
        server_ == nullptr ? 0 : server_->entities().stats().live_entities;
    teardown_report_.physics_bodies_before = server_ == nullptr ? 0 : server_->physics_body_count();
    if (server_ != nullptr) {
        const auto& collision = server_->chunk_collision().stats();
        teardown_report_.session_jobs_before = collision.pending_chunk_count +
                                               collision.in_flight_job_count +
                                               collision.completed_mailbox_count;
    }
    teardown_report_.registered_cleanup_count = cleanup_entries_.size();
    (void)request_stop();
    auto result = core::Status::ok();
    const auto remember_failure = [&result](const core::Status& status) {
        if (!status && result) {
            result = status;
        }
    };
    auto transport_status = core::Status::ok();
    if (remote_transport_ != nullptr) {
        transport_status = remote_transport_->disconnect(last_tick_time_ms_);
    } else if (server_ != nullptr && client_ != nullptr && server_->is_running()) {
        transport_status = server_->disconnect_client(client_->client_id());
    }
    remember_failure(transport_status);
    teardown_report_.transport_stopped = static_cast<bool>(transport_status);
    auto server_status = core::Status::ok();
    if (server_ != nullptr && server_->is_running()) {
        server_status = server_->stop();
        remember_failure(server_status);
    }
    teardown_report_.authoritative_ticking_stopped = static_cast<bool>(server_status);

    std::vector<CleanupEntry> failed_cleanup_entries;
    for (auto entry = cleanup_entries_.rbegin(); entry != cleanup_entries_.rend(); ++entry) {
        auto cleanup_status = entry->cleanup();
        remember_failure(cleanup_status);
        if (cleanup_status) {
            ++teardown_report_.completed_cleanup_count;
        } else {
            failed_cleanup_entries.push_back(*entry);
        }
    }
    std::ranges::reverse(failed_cleanup_entries);
    cleanup_entries_ = std::move(failed_cleanup_entries);
    presentation_synchronizer_.clear();
    presentation_.clear();
    teardown_report_.presentation_objects_after = presentation_.stats().retained_object_count;
    teardown_report_.presentation_cleared = true;
    if (transport_status) {
        client_.reset();
        remote_transport_.reset();
        teardown_report_.client_destroyed = true;
    }
    if (server_status) {
        server_.reset();
        teardown_report_.server_destroyed = true;
    }
    teardown_report_.server_entities_after =
        server_ == nullptr ? 0 : server_->entities().stats().live_entities;
    teardown_report_.physics_bodies_after = server_ == nullptr ? 0 : server_->physics_body_count();
    if (server_ == nullptr) {
        teardown_report_.session_jobs_after = 0;
    } else {
        const auto& collision = server_->chunk_collision().stats();
        teardown_report_.session_jobs_after = collision.pending_chunk_count +
                                              collision.in_flight_job_count +
                                              collision.completed_mailbox_count;
    }
    accepting_commands_ = false;
    state_ = result ? RuntimeSessionState::stopped : RuntimeSessionState::stopping;
    return result;
}

bool RuntimeSession::is_running() const noexcept {
    return state_ == RuntimeSessionState::running;
}

bool RuntimeSession::accepts_commands() const noexcept {
    return accepting_commands_ && state_ == RuntimeSessionState::running;
}

std::uint64_t RuntimeSession::ownership_generation() const noexcept {
    return request_.ownership_generation;
}

RuntimeSessionState RuntimeSession::state() const noexcept {
    return state_;
}

SessionConnectionState RuntimeSession::connection_state() const noexcept {
    if (state_ == RuntimeSessionState::stopping) {
        return SessionConnectionState::disconnecting;
    }
    if (state_ == RuntimeSessionState::stopped) {
        return SessionConnectionState::disconnected;
    }
    if (client_ == nullptr) {
        return SessionConnectionState::none;
    }
    if (client_->is_connected()) {
        return SessionConnectionState::connected;
    }
    if (remote_transport_ != nullptr &&
        remote_transport_->state() == net::TransportClientState::connecting) {
        return SessionConnectionState::connecting;
    }
    return SessionConnectionState::disconnected;
}

const SessionLaunchRequest& RuntimeSession::launch_request() const noexcept {
    return request_;
}

const SessionTeardownReport& RuntimeSession::teardown_report() const noexcept {
    return teardown_report_;
}

SessionResourceCounts RuntimeSession::resource_counts() const noexcept {
    SessionResourceCounts counts;
    counts.server_entities = server_ == nullptr ? 0 : server_->entities().stats().live_entities;
    counts.physics_bodies = server_ == nullptr ? 0 : server_->physics_body_count();
    counts.presentation_objects = presentation_.stats().retained_object_count;
    counts.registered_cleanup_callbacks = cleanup_entries_.size();
    if (server_ != nullptr) {
        const auto& collision = server_->chunk_collision().stats();
        counts.active_jobs = collision.pending_chunk_count + collision.in_flight_job_count +
                             collision.completed_mailbox_count;
    }
    return counts;
}

ServerRuntime* RuntimeSession::server() noexcept {
    return server_.get();
}

const ServerRuntime* RuntimeSession::server() const noexcept {
    return server_.get();
}

ClientRuntime* RuntimeSession::client() noexcept {
    return client_.get();
}

const ClientRuntime* RuntimeSession::client() const noexcept {
    return client_.get();
}

PresentationWorld* RuntimeSession::presentation() noexcept {
    return client_ == nullptr ? nullptr : &presentation_;
}

const PresentationWorld* RuntimeSession::presentation() const noexcept {
    return client_ == nullptr ? nullptr : &presentation_;
}

const RuntimeConfiguration& RuntimeSession::config() const noexcept {
    return config_;
}

std::uint64_t RuntimeSession::frame_count() const noexcept {
    return frame_count_;
}

std::uint64_t RuntimeSession::fixed_step_tick() const noexcept {
    return fixed_step_.tick();
}

const std::optional<RuntimeFrameStats>& RuntimeSession::last_frame_stats() const noexcept {
    return last_frame_stats_;
}

const std::optional<core::Error>& RuntimeSession::fault() const noexcept {
    return fault_;
}

std::string_view session_mode_name(SessionMode mode) noexcept {
    switch (mode) {
    case SessionMode::local_single_player:
        return "local-single-player";
    case SessionMode::hosted_multiplayer:
        return "hosted-multiplayer";
    case SessionMode::remote_multiplayer:
        return "remote-multiplayer";
    case SessionMode::dedicated_server:
        return "dedicated-server";
    case SessionMode::automated:
        return "automated";
    case SessionMode::replay:
        return "replay";
    }
    return "unknown";
}

std::string_view world_source_kind_name(WorldSourceKind source) noexcept {
    switch (source) {
    case WorldSourceKind::generated:
        return "generated";
    case WorldSourceKind::existing_save:
        return "existing-save";
    case WorldSourceKind::developer_scenario:
        return "developer-scenario";
    case WorldSourceKind::packaged_fixture:
        return "packaged-fixture";
    case WorldSourceKind::remote_server:
        return "remote-server";
    case WorldSourceKind::automated_scenario:
        return "automated-scenario";
    case WorldSourceKind::replay:
        return "replay";
    }
    return "unknown";
}

std::string_view persistence_policy_name(PersistencePolicy policy) noexcept {
    switch (policy) {
    case PersistencePolicy::ephemeral:
        return "ephemeral";
    case PersistencePolicy::temporary_copy:
        return "temporary-copy";
    case PersistencePolicy::persistent:
        return "persistent";
    }
    return "unknown";
}

std::string_view runtime_session_state_name(RuntimeSessionState state) noexcept {
    switch (state) {
    case RuntimeSessionState::created:
        return "created";
    case RuntimeSessionState::starting:
        return "starting";
    case RuntimeSessionState::running:
        return "running";
    case RuntimeSessionState::stopping:
        return "stopping";
    case RuntimeSessionState::stopped:
        return "stopped";
    case RuntimeSessionState::faulted:
        return "faulted";
    }
    return "unknown";
}

std::string_view session_connection_state_name(SessionConnectionState state) noexcept {
    switch (state) {
    case SessionConnectionState::none:
        return "none";
    case SessionConnectionState::connecting:
        return "connecting";
    case SessionConnectionState::connected:
        return "connected";
    case SessionConnectionState::disconnecting:
        return "disconnecting";
    case SessionConnectionState::disconnected:
        return "disconnected";
    }
    return "unknown";
}

std::string_view session_startup_phase_name(SessionStartupPhase phase) noexcept {
    switch (phase) {
    case SessionStartupPhase::validating_request:
        return "Validating session request";
    case SessionStartupPhase::initializing_content:
        return "Preparing content services";
    case SessionStartupPhase::reading_world:
        return "Reading world snapshot";
    case SessionStartupPhase::restoring_world:
        return "Restoring saved world state";
    case SessionStartupPhase::preparing_world:
        return "Preparing world state";
    case SessionStartupPhase::initializing_physics:
        return "Initializing world physics";
    case SessionStartupPhase::generating_spawn_area:
        return "Generating the spawn area";
    case SessionStartupPhase::registering_gameplay_systems:
        return "Registering gameplay systems";
    case SessionStartupPhase::starting_authoritative_server:
        return "Starting authoritative server";
    case SessionStartupPhase::starting_client:
        return "Starting client runtime";
    case SessionStartupPhase::connecting_transport:
        return "Connecting transport";
    case SessionStartupPhase::constructing_presentation:
        return "Constructing presentation world";
    case SessionStartupPhase::ready:
        return "World ready";
    case SessionStartupPhase::cancelling:
        return "Cancelling session launch";
    }
    return "Loading world";
}

bool session_mode_is_multiplayer(SessionMode mode) noexcept {
    return mode == SessionMode::hosted_multiplayer || mode == SessionMode::remote_multiplayer;
}

} // namespace heartstead::game
