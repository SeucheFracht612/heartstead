#include "game/runtime/runtime_session.hpp"

#include "engine/core/hash.hpp"
#include "engine/scenarios/scenario_prototype.hpp"
#include "engine/net/command_payload.hpp"
#include "engine/world/world_snapshot.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace heartstead::game {

namespace {

[[nodiscard]] std::string
content_session_fingerprint(const save::SaveMetadata& metadata) {
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
        max_transient_snapshot_payload_bytes_per_tick == 0) {
        return core::Status::failure(
            "runtime_configuration.invalid_replication_budget",
            "transient replication message and byte budgets must be non-zero");
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
            return core::Status::failure(
                "runtime_configuration.remote_endpoint_missing",
                "remote-client-only runtime requires a server endpoint");
        }
        if (remote_server_endpoint.has_value()) {
            status = net::validate_transport_endpoint(*remote_server_endpoint);
            if (!status || remote_server_endpoint->port == 0) {
                return !status
                           ? status
                           : core::Status::failure(
                                 "runtime_configuration.remote_endpoint_port",
                                 "remote server endpoint port must be non-zero");
            }
        }
    }
    if (create_renderer && (!create_client || headless)) {
        return core::Status::failure("runtime_configuration.invalid_renderer",
                                     "renderer creation requires a non-headless client runtime");
    }
    if (create_audio && (!create_client || headless)) {
        return core::Status::failure("runtime_configuration.invalid_audio",
                                     "audio creation requires a non-headless client runtime");
    }
    return core::Status::ok();
}

RuntimeSession::RuntimeSession(RuntimeConfiguration config, SessionRequest request,
                               const modding::PrototypeRegistry& prototypes,
                               const world::VoxelPalette& voxel_palette)
    : config_(std::move(config)), request_(std::move(request)), prototypes_(&prototypes),
      voxel_palette_(&voxel_palette), fixed_step_(config_.fixed_step) {}

RuntimeSession::~RuntimeSession() {
    (void)shutdown();
}

core::Result<std::unique_ptr<RuntimeSession>>
RuntimeSession::create(RuntimeConfiguration config, SessionRequest request,
                       const modding::PrototypeRegistry& prototypes,
                       const world::VoxelPalette& voxel_palette) {
    auto status = config.validate();
    if (!status) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(status.error().code,
                                                                      status.error().message);
    }
    if (request.initial_snapshot.has_value()) {
        request.metadata = request.initial_snapshot->metadata;
    }
    if (!request.metadata.validate()) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(
            "runtime_session.invalid_metadata", "session save metadata is invalid");
    }
    if (request.scenario_id.empty()) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(
            "runtime_session.invalid_scenario", "session scenario id must not be empty");
    }
    auto session = std::unique_ptr<RuntimeSession>(
        new RuntimeSession(std::move(config), std::move(request), prototypes, voxel_palette));
    status = session->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<RuntimeSession>>::failure(status.error().code,
                                                                      status.error().message);
    }
    return core::Result<std::unique_ptr<RuntimeSession>>::success(std::move(session));
}

core::Status RuntimeSession::initialize() {
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
    }

    if (config_.create_server) {
        ServerRuntimeDesc server_desc;
        server_desc.world.metadata = request_.metadata;
        server_desc.world.voxel_palette = voxel_palette_->manifest();
        server_desc.host.transport.backend = config_.use_in_memory_transport
                                                 ? net::TransportBackend::in_memory
                                                 : net::TransportBackend::external_library;
        server_desc.host.transport.external.bind_endpoint = config_.server_bind_endpoint;
        server_desc.host.transport.external.content_fingerprint =
            content_session_fingerprint(request_.metadata);
        server_desc.physics.backend = config_.physics_backend;
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
        auto server = ServerRuntime::create(std::move(server_desc));
        if (!server) {
            return core::Status::failure(server.error().code, server.error().message);
        }
        server_ = std::move(server).value();
        auto status = server_->start();
        if (!status) {
            return status;
        }
    }

    if (config_.create_client) {
        world::WorldStateDesc client_world;
        client_world.metadata = request_.metadata;
        client_world.voxel_palette = voxel_palette_->manifest();
        if (config_.use_in_memory_transport) {
            if (server_ == nullptr) {
                return core::Status::failure(
                    "runtime_session.local_server_missing",
                    "in-memory client requires a local authoritative server");
            }
            auto connected = server_->connect_client();
            if (!connected) {
                return core::Status::failure(connected.error().code,
                                             connected.error().message);
            }
            client_ = std::make_unique<ClientRuntime>(
                connected.value(), std::move(client_world),
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
            transport_config.content_fingerprint =
                content_session_fingerprint(request_.metadata);
            auto remote =
                net::create_external_transport_client(std::move(transport_config));
            if (!remote) {
                return core::Status::failure(remote.error().code, remote.error().message);
            }
            remote_transport_ = std::move(remote).value();
            auto status = remote_transport_->connect(0);
            if (!status) {
                return status;
            }
            client_ = std::make_unique<ClientRuntime>(
                core::NetId{}, std::move(client_world),
                server_ == nullptr ? nullptr : &server_->replication_registry(),
                voxel_palette_);
        }
        auto status = pump_client_messages(0);
        if (!status) {
            return status;
        }
        if (config_.use_in_memory_transport && !client_->is_connected()) {
            return core::Status::failure(
                "runtime_session.client_handshake_failed",
                "local client did not accept the server welcome");
        }
        auto synchronized = client_->synchronize();
        if (!synchronized) {
            return core::Status::failure(synchronized.error().code, synchronized.error().message);
        }
        auto presented = synchronize_presentation();
        if (!presented) {
            return core::Status::failure(presented.error().code, presented.error().message);
        }
    }
    running_ = true;
    return core::Status::ok();
}

core::Result<RuntimeFrameStats> RuntimeSession::run_frame(RuntimeFrameInput input) {
    if (fault_.has_value()) {
        return core::Result<RuntimeFrameStats>::failure("runtime_session.faulted",
                                                        "runtime session cannot continue after '" +
                                                            fault_->code + "': " + fault_->message);
    }
    if (!running_) {
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
    stats.server_ticks.reserve(frame.value().step_count);
    for (std::uint32_t step = 0; step < frame.value().step_count; ++step) {
        if (server_ != nullptr) {
            const auto tick = frame.value().first_tick + step;
            auto tick_result = server_->run_tick(
                tick, 1.0 / static_cast<double>(config_.fixed_step.ticks_per_second), input.now_ms);
            if (!tick_result) {
                return fault_frame(tick_result.error());
            }
            stats.server_ticks.push_back(std::move(tick_result).value());
            stats.authoritative_world_tick = server_->world().world_time();
        }
        auto status = pump_client_messages(input.now_ms);
        if (!status) {
            return fault_frame(status.error());
        }
        if (client_ != nullptr) {
            auto synchronized = client_->synchronize(frame.value().first_tick + step);
            if (!synchronized) {
                return fault_frame(synchronized.error());
            }
            stats.client = std::move(synchronized).value();
            auto presented = synchronize_presentation();
            if (!presented) {
                return fault_frame(presented.error());
            }
            stats.presentation.inserted_objects += presented.value().inserted_objects;
            stats.presentation.adapter_count += presented.value().adapter_count;
            stats.presentation.updated_objects += presented.value().updated_objects;
            stats.presentation.removed_objects += presented.value().removed_objects;
            stats.presentation.unchanged_objects += presented.value().unchanged_objects;
        }
    }
    last_frame_stats_ = stats;
    ++frame_count_;
    return core::Result<RuntimeFrameStats>::success(std::move(stats));
}

core::Result<RuntimeFrameStats> RuntimeSession::fault_frame(const core::Error& error) {
    fault_ = error;
    running_ = false;
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
RuntimeSession::submit_tracked_command(std::string type, std::string payload,
                                       std::int64_t now_ms) {
    if (!running_ || client_ == nullptr ||
        (server_ == nullptr && remote_transport_ == nullptr)) {
        return core::Result<std::uint64_t>::failure(
            "runtime_session.command_path_unavailable",
            "commands require an active client and authoritative server connection");
    }
    auto command = client_->create_command(std::move(type), std::move(payload), now_ms);
    if (!command) {
        return core::Result<std::uint64_t>::failure(command.error().code,
                                                    command.error().message);
    }
    const auto sequence = command.value().sequence;
    auto status = remote_transport_ != nullptr
                      ? remote_transport_->send_to_server(net::make_command_transport_message(
                            command.value()))
                      : server_->submit_command(client_->client_id(),
                                                std::move(command).value());
    if (!status) {
        return core::Result<std::uint64_t>::failure(status.error().code,
                                                    status.error().message);
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
        if (predicted != nullptr &&
            network_input.tick <= predicted->state.simulation_tick) {
            if (predicted->state.simulation_tick ==
                std::numeric_limits<std::uint64_t>::max()) {
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
    if (!running_ || client_ == nullptr ||
        (server_ == nullptr && remote_transport_ == nullptr) ||
        !client_->is_connected()) {
        return core::Status::failure(
            "runtime_session.movement_path_unavailable",
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
    return client_->predict_local_input(network_input);
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
    if (!running_ || server_ == nullptr) {
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
    return presentation_.extract(fixed_step_.tick());
}

core::Status RuntimeSession::pump_client_messages(std::int64_t now_ms) {
    if (client_ == nullptr) {
        return core::Status::ok();
    }
    if (remote_transport_ != nullptr) {
        auto maintenance = remote_transport_->poll_maintenance(now_ms);
        if (!maintenance) {
            return core::Status::failure(maintenance.error().code,
                                         maintenance.error().message);
        }
        if (maintenance.value().disconnected) {
            const auto reason =
                maintenance.value().disconnect_reason_code.empty()
                    ? std::string("transport_client.disconnected")
                    : maintenance.value().disconnect_reason_code;
            client_->session().mark_transport_disconnected(
                reason, "remote transport disconnected", now_ms);
            return core::Status::failure(reason, "remote transport disconnected");
        }
        const auto messages = remote_transport_->drain_server_messages();
        auto status = client_->receive(messages);
        if (!status || client_->is_connected() ||
            remote_transport_->state() != net::TransportClientState::disconnected) {
            return status;
        }
        const auto session_stats = client_->session().stats();
        return core::Status::failure(
            session_stats.disconnect_reason_code.empty()
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
    return !messages
               ? core::Status::failure(messages.error().code, messages.error().message)
               : client_->receive(messages.value());
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

core::Status RuntimeSession::shutdown() {
    if (!running_ && server_ == nullptr && client_ == nullptr) {
        return core::Status::ok();
    }
    auto result = core::Status::ok();
    if (remote_transport_ != nullptr) {
        auto status = remote_transport_->disconnect(0);
        if (!status) {
            result = status;
        }
    } else if (server_ != nullptr && client_ != nullptr && server_->is_running()) {
        auto status = server_->disconnect_client(client_->client_id());
        if (!status) {
            result = status;
        }
    }
    presentation_synchronizer_.clear();
    presentation_.clear();
    client_.reset();
    remote_transport_.reset();
    if (server_ != nullptr) {
        auto status = server_->stop();
        if (!status && result) {
            result = status;
        }
    }
    server_.reset();
    running_ = false;
    return result;
}

bool RuntimeSession::is_running() const noexcept {
    return running_;
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

const std::optional<RuntimeFrameStats>& RuntimeSession::last_frame_stats() const noexcept {
    return last_frame_stats_;
}

const std::optional<core::Error>& RuntimeSession::fault() const noexcept {
    return fault_;
}

} // namespace heartstead::game
