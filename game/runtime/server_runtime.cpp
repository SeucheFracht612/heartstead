#include "game/runtime/server_runtime.hpp"

#include "engine/cargo/cargo_prototype.hpp"
#include "engine/entities/entity_prototype.hpp"
#include "engine/items/item_prototype.hpp"
#include "engine/movement/movement_prediction.hpp"
#include "engine/profiling/profiler.hpp"
#include "engine/simulation/fire_prototype.hpp"
#include "engine/world/chunks/chunk_edit_delta_codec.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "engine/world/voxel_change.hpp"
#include "engine/world/world_commands.hpp"
#include "engine/world/world_snapshot.hpp"
#include "game/features/interaction/voxel_commands.hpp"
#include "game/features/interaction/voxel_interaction_module.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/scenarios/scenario_setup.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace heartstead::game {

namespace {

using ReplicationClock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t
elapsed_replication_microseconds(ReplicationClock::time_point started) noexcept {
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::nanoseconds>(ReplicationClock::now() - started)
            .count();
    if (elapsed <= 0) {
        return 1;
    }
    const auto nanoseconds = static_cast<std::uint64_t>(elapsed);
    return 1 + (nanoseconds - 1) / 1'000;
}

template <typename Snapshot, typename Encoder, typename Sender>
[[nodiscard]] core::Status replicate_transient_snapshot_candidates(
    net::ReplicationTickBudget& budget, const std::vector<std::uint64_t>& recipients,
    const std::vector<Snapshot>& snapshots, Encoder&& encode, Sender&& send) {
    if (recipients.empty() || snapshots.empty()) {
        return core::Status::ok();
    }
    std::vector<net::ReplicationBudgetLimit> preparation_limits(recipients.size());
    for (const auto& snapshot : snapshots) {
        bool has_serialization_recipient = false;
        for (std::size_t index = 0; index < recipients.size(); ++index) {
            auto limit = budget.preparation_limit(core::NetId::from_value(recipients[index]));
            if (!limit) {
                return core::Status::failure(limit.error().code, limit.error().message);
            }
            has_serialization_recipient |= limit.value() == net::ReplicationBudgetLimit::none;
            preparation_limits[index] = limit.value();
        }
        if (!has_serialization_recipient) {
            for (std::size_t index = 0; index < recipients.size(); ++index) {
                auto status = budget.record_deferred(core::NetId::from_value(recipients[index]),
                                                     preparation_limits[index]);
                if (!status) {
                    return status;
                }
            }
            continue;
        }

        auto began_serialization = budget.begin_shared_serialization();
        if (!began_serialization) {
            return core::Status::failure(began_serialization.error().code,
                                         began_serialization.error().message);
        }
        if (!began_serialization.value()) {
            for (std::size_t index = 0; index < recipients.size(); ++index) {
                const auto limit = preparation_limits[index] == net::ReplicationBudgetLimit::none
                                       ? net::ReplicationBudgetLimit::global_serialization_time
                                       : preparation_limits[index];
                auto status =
                    budget.record_deferred(core::NetId::from_value(recipients[index]), limit);
                if (!status) {
                    return status;
                }
            }
            continue;
        }

        const auto started = ReplicationClock::now();
        const auto payload = encode(snapshot);
        const auto serialization_time_us = elapsed_replication_microseconds(started);
        auto serialization_status = budget.finish_shared_serialization(serialization_time_us);
        if (!serialization_status) {
            return serialization_status;
        }

        for (std::size_t index = 0; index < recipients.size(); ++index) {
            const auto recipient = core::NetId::from_value(recipients[index]);
            if (preparation_limits[index] != net::ReplicationBudgetLimit::none) {
                auto status = budget.record_deferred(recipient, preparation_limits[index]);
                if (!status) {
                    return status;
                }
                continue;
            }
            auto admission = budget.admit_prepared(
                recipient, static_cast<std::uint64_t>(payload.size()), serialization_time_us);
            if (!admission) {
                return core::Status::failure(admission.error().code, admission.error().message);
            }
            if (!admission.value()) {
                continue;
            }
            auto status = send(recipient, payload);
            if (!status) {
                return status;
            }
        }
    }
    return core::Status::ok();
}

class RendererProofChunkLoadGenerator final : public world::IChunkLoadGenerator {
  public:
    explicit RendererProofChunkLoadGenerator(scenarios::RendererProofVoxelTypes types) noexcept
        : types_(types) {}

    [[nodiscard]] core::Result<world::VoxelChunk> generate(world::ChunkCoord coord) const override {
        return scenarios::generate_renderer_proof_chunk(coord, types_);
    }

  private:
    scenarios::RendererProofVoxelTypes types_;
};

[[nodiscard]] core::Status startup_cancelled(std::stop_token stop_token) {
    if (!stop_token.stop_requested()) {
        return core::Status::ok();
    }
    return core::Status::failure("session_startup.cancelled", "session startup was cancelled");
}

void grant_private_subject_access(net::HostSession& host, core::NetId client_id,
                                  core::SaveId subject_id) {
    auto policy = host.replication_relevance_policy();
    auto rule = std::ranges::find_if(policy.private_access_rules,
                                     [client_id](const net::ReplicationPrivateAccessRule& value) {
                                         return value.client_id == client_id;
                                     });
    if (rule == policy.private_access_rules.end()) {
        policy.private_access_rules.push_back({client_id, {subject_id}});
    } else if (std::ranges::find(rule->private_subjects, subject_id) ==
               rule->private_subjects.end()) {
        rule->private_subjects.push_back(subject_id);
    }
    host.set_replication_relevance_policy(std::move(policy));
}

void revoke_private_subject_access(net::HostSession& host, core::NetId client_id,
                                   core::SaveId subject_id) {
    auto policy = host.replication_relevance_policy();
    auto rule = std::ranges::find_if(policy.private_access_rules,
                                     [client_id](const net::ReplicationPrivateAccessRule& value) {
                                         return value.client_id == client_id;
                                     });
    if (rule == policy.private_access_rules.end()) {
        return;
    }
    std::erase(rule->private_subjects, subject_id);
    if (rule->private_subjects.empty()) {
        policy.private_access_rules.erase(rule);
    }
    host.set_replication_relevance_policy(std::move(policy));
}

[[nodiscard]] bool collision_geometry_matches(world::VoxelCell lhs, world::VoxelCell rhs,
                                              const world::VoxelPalette& palette) noexcept {
    if (lhs.type == rhs.type) {
        return true;
    }
    const auto* lhs_definition = palette.find_by_type(lhs.type);
    const auto* rhs_definition = palette.find_by_type(rhs.type);
    const auto bounds_match = [](const world::VoxelDefinition* first,
                                 const world::VoxelDefinition* second) {
        const auto first_size = first == nullptr ? std::size_t{0} : first->collision_bounds.size();
        const auto second_size =
            second == nullptr ? std::size_t{0} : second->collision_bounds.size();
        if (first_size != second_size) {
            return false;
        }
        for (std::size_t index = 0; index < first_size; ++index) {
            const auto& first_bounds = first->collision_bounds[index];
            const auto& second_bounds = second->collision_bounds[index];
            if (first_bounds.min != second_bounds.min || first_bounds.max != second_bounds.max) {
                return false;
            }
        }
        return true;
    };
    return bounds_match(lhs_definition, rhs_definition);
}

[[nodiscard]] double point_interval_distance(double point, double minimum,
                                             double maximum) noexcept {
    return point < minimum ? minimum - point : point > maximum ? point - maximum : 0.0;
}

[[nodiscard]] double interval_distance(double first_minimum, double first_maximum,
                                       double second_minimum, double second_maximum) noexcept {
    return first_maximum < second_minimum   ? second_minimum - first_maximum
           : second_maximum < first_minimum ? first_minimum - second_maximum
                                            : 0.0;
}

[[nodiscard]] bool capsule_overlaps_bounds(math::Vec3d feet, movement::CharacterShape shape,
                                           math::Bounds3d bounds) noexcept {
    const auto radius = shape.width * 0.5;
    const auto dx = point_interval_distance(feet.x, bounds.min.x, bounds.max.x);
    const auto dz = point_interval_distance(feet.z, bounds.min.z, bounds.max.z);
    const auto dy = interval_distance(feet.y + radius, feet.y + shape.height - radius, bounds.min.y,
                                      bounds.max.y);
    return dx * dx + dy * dy + dz * dz < radius * radius;
}

[[nodiscard]] core::Result<std::vector<world::VoxelLightSource>>
collect_fire_light_sources(const world::WorldState& state,
                           const modding::PrototypeRegistry& prototypes) {
    std::vector<world::VoxelLightSource> sources;
    for (const auto* fire : state.fires().records()) {
        if (!fire->emits_light()) {
            continue;
        }
        const auto* prototype = prototypes.find(fire->prototype_id);
        if (prototype == nullptr) {
            return core::Result<std::vector<world::VoxelLightSource>>::failure(
                "server_runtime.fire_prototype_missing",
                "active fire references a prototype that is not loaded");
        }
        auto definition = simulation::fire_definition_from_prototype(*prototype);
        if (!definition) {
            return core::Result<std::vector<world::VoxelLightSource>>::failure(
                definition.error().code, definition.error().message);
        }

        const world::WorldPosition* position = nullptr;
        if (const auto* build_piece = state.build_objects().find(fire->fire_id);
            build_piece != nullptr) {
            position = &build_piece->transform.position;
        } else if (const auto* entity = state.entities().find_by_save_id(fire->fire_id);
                   entity != nullptr) {
            position = &entity->transform.position;
        } else if (const auto* resource = state.physical_resources().find(fire->fire_id);
                   resource != nullptr) {
            position = &resource->position;
        }
        if (position == nullptr) {
            continue;
        }

        auto light = static_cast<std::uint16_t>(definition.value().light_level);
        if (light <= 15) {
            light *= 17;
        }
        sources.push_back(
            {position->anchor, static_cast<std::uint8_t>(std::min<std::uint16_t>(light, 255))});
    }
    std::ranges::sort(sources, {}, &world::VoxelLightSource::position);
    std::vector<world::VoxelLightSource> coalesced;
    coalesced.reserve(sources.size());
    for (const auto source : sources) {
        if (!coalesced.empty() && coalesced.back().position == source.position) {
            coalesced.back().light = std::max(coalesced.back().light, source.light);
        } else {
            coalesced.push_back(source);
        }
    }
    return core::Result<std::vector<world::VoxelLightSource>>::success(std::move(coalesced));
}

} // namespace

ServerRuntime::ServerRuntime(ServerRuntimeDesc desc)
    : desc_(std::move(desc)), transient_replication_budget_(desc_.transient_replication_budget),
      world_(desc_.world), host_(desc_.host) {}

core::Result<std::unique_ptr<ServerRuntime>> ServerRuntime::create(ServerRuntimeDesc desc) {
    if (desc.prototypes == nullptr || desc.voxel_palette == nullptr) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            "server_runtime.missing_content",
            "authoritative runtime requires immutable prototype and voxel registries");
    }
    auto scenario_status = desc.scenario.validate();
    if (!scenario_status) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            scenario_status.error().code, scenario_status.error().message);
    }
    if (desc.simulation_ticks_per_second == 0 || desc.simulation_ticks_per_second > 1000) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            "server_runtime.invalid_simulation_rate",
            "authoritative simulation rate must be between 1 and 1000 Hz");
    }
    auto replication_budget_status = desc.transient_replication_budget.validate();
    if (!replication_budget_status) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            replication_budget_status.error().code, replication_budget_status.error().message);
    }
    auto chunk_subscription_status = desc.chunk_subscriptions.validate();
    if (!chunk_subscription_status) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            chunk_subscription_status.error().code, chunk_subscription_status.error().message);
    }
    auto chunk_streaming_status = desc.chunk_streaming.validate();
    if (!chunk_streaming_status) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            chunk_streaming_status.error().code, chunk_streaming_status.error().message);
    }
    if (desc.max_chunk_snapshot_serialization_time_us_per_tick == 0) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            "server_runtime.invalid_chunk_snapshot_serialization_budget",
            "chunk snapshot serialization time budget must be non-zero");
    }
    auto world_time_status = desc.world_time.validate();
    if (!world_time_status) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(
            world_time_status.error().code, world_time_status.error().message);
    }
    auto runtime = std::unique_ptr<ServerRuntime>(new ServerRuntime(std::move(desc)));
    auto status = runtime->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<ServerRuntime>>::failure(status.error().code,
                                                                     status.error().message);
    }
    return core::Result<std::unique_ptr<ServerRuntime>>::success(std::move(runtime));
}

core::Status ServerRuntime::initialize() {
    auto cancellation = startup_cancelled(desc_.stop_token);
    if (!cancellation) {
        return cancellation;
    }
    if (desc_.initial_snapshot.has_value()) {
        if (desc_.startup_progress) {
            desc_.startup_progress(ServerRuntimeStartupPhase::restoring_world);
        }
        auto state_snapshot = *desc_.initial_snapshot;
        const auto saved_chunk_edits = std::move(state_snapshot.chunk_edits);
        state_snapshot.chunk_edits.clear();
        auto imported = world::WorldSnapshotBridge::import_validated_snapshot(state_snapshot,
                                                                              *desc_.prototypes);
        if (!imported) {
            return core::Status::failure(imported.error().code, imported.error().message);
        }
        world_ = std::move(imported).value();
        for (const auto& saved_chunk : saved_chunk_edits) {
            cancellation = startup_cancelled(desc_.stop_token);
            if (!cancellation) {
                return cancellation;
            }
            auto edits = world::ChunkEditDeltaTextCodec::decode(saved_chunk.coord,
                                                                saved_chunk.encoded_edit_delta);
            if (!edits) {
                return core::Status::failure(edits.error().code, edits.error().message);
            }
            pending_saved_voxel_edits_.insert(pending_saved_voxel_edits_.end(),
                                              std::make_move_iterator(edits.value().begin()),
                                              std::make_move_iterator(edits.value().end()));
        }
    }
    if (desc_.startup_progress) {
        desc_.startup_progress(ServerRuntimeStartupPhase::initializing_physics);
    }
    cancellation = startup_cancelled(desc_.stop_token);
    if (!cancellation) {
        return cancellation;
    }
    auto physics = physics::create_physics_world(desc_.physics);
    if (!physics) {
        return core::Status::failure(physics.error().code, physics.error().message);
    }
    physics_ = std::move(physics).value();

    if (desc_.startup_progress) {
        desc_.startup_progress(ServerRuntimeStartupPhase::generating_spawn_area);
    }
    if (desc_.initial_snapshot.has_value()) {
        auto spawn_status = ensure_spawn_area();
        if (!spawn_status) {
            return spawn_status;
        }
    } else {
        auto scenario_status = initialize_new_world_scenario();
        if (!scenario_status) {
            return scenario_status;
        }
    }
    if (desc_.scenario.setup_hook == "renderer_proof") {
        auto types = scenarios::resolve_renderer_proof_voxel_types(*desc_.voxel_palette);
        if (!types) {
            return core::Status::failure(types.error().code, types.error().message);
        }
        world::ChunkLoadSchedulerContext load_context;
        load_context.generator = std::make_shared<RendererProofChunkLoadGenerator>(types.value());
        auto loader =
            world::ChunkLoadScheduler::create(std::move(load_context), desc_.chunk_loading);
        if (!loader) {
            return core::Status::failure(loader.error().code, loader.error().message);
        }
        chunk_loader_ = std::move(loader).value();
        renderer_proof_streaming_enabled_ = true;
    }
    auto chunk_collision = physics::ChunkCollisionSystem::create(*physics_, *desc_.voxel_palette,
                                                                 desc_.chunk_collision);
    if (!chunk_collision) {
        return core::Status::failure(chunk_collision.error().code, chunk_collision.error().message);
    }
    chunk_collision_ = std::move(chunk_collision).value();
    desc_.physical_resources.physics_island = desc_.chunk_collision.physics_island;
    auto physical_resources =
        physics::PhysicalResourcePhysicsSystem::create(*physics_, desc_.physical_resources);
    if (!physical_resources) {
        return core::Status::failure(physical_resources.error().code,
                                     physical_resources.error().message);
    }
    physical_resource_physics_ = std::move(physical_resources).value();
    auto chunk_fluids = world::ChunkFluidSystem::create(*desc_.voxel_palette, desc_.chunk_fluids);
    if (!chunk_fluids) {
        return core::Status::failure(chunk_fluids.error().code, chunk_fluids.error().message);
    }
    chunk_fluids_ = std::move(chunk_fluids).value();
    auto chunk_lighting =
        world::ChunkLightSystem::create(*desc_.voxel_palette, desc_.chunk_lighting);
    if (!chunk_lighting) {
        return core::Status::failure(chunk_lighting.error().code, chunk_lighting.error().message);
    }
    chunk_lighting_ = std::move(chunk_lighting).value();

    cancellation = startup_cancelled(desc_.stop_token);
    if (!cancellation) {
        return cancellation;
    }
    if (desc_.startup_progress) {
        desc_.startup_progress(ServerRuntimeStartupPhase::registering_gameplay_systems);
    }

    auto status = entities_.register_cleanup(
        "runtime.entity_motion_replication",
        [this](entities::EntityId id, const entities::EntityWorldRecord&) {
            const auto* identity = entities_.find_component<entities::NetworkIdentityComponent>(id);
            if (identity != nullptr && identity->net_id.is_valid()) {
                pending_entity_motion_removals_.push_back(identity->net_id);
            }
            return core::Status::ok();
        });
    if (!status) {
        return status;
    }
    status = world::WorldCommandRegistry::register_engine_commands(commands_);
    if (!status) {
        return status;
    }
    status = commands_.register_command(net::CommandDescriptor{
        "player.input",
        false,
        true,
        [this](const net::CommandEnvelope& command, const net::CommandExecutionContext&,
               world::WorldOperation&) {
            const auto found = player_connections_.find(command.sender.value());
            if (found == player_connections_.end()) {
                return core::Status::failure("server_runtime.player_not_connected",
                                             "movement input sender has no active player");
            }
            auto input = movement::PlayerInputTextCodec::decode(command.payload);
            if (!input) {
                return core::Status::failure(input.error().code, input.error().message);
            }
            return found->second.pending_inputs.push(std::move(input).value());
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.command_gateway",
        simulation::SimulationPhase::commands,
        {},
        [this](simulation::SimulationContext& context) {
            net::CommandExecutionContext command_context;
            command_context.executor_role = net::CommandExecutorRole::authoritative_server;
            command_context.server_time_ms = current_time_ms_;
            command_context.save_ids = &world_.save_ids();
            command_context.prototypes = desc_.prototypes;
            command_context.world_state = &world_;
            command_context.voxel_palette = desc_.voxel_palette;
            auto result = host_.tick(commands_, command_context);
            if (!result) {
                return core::Status::failure(result.error().code, result.error().message);
            }
            current_commands_ = std::move(result).value();
            std::uint64_t spatial_event_count = 0;
            std::uint64_t relevant_spatial_delivery_count = 0;
            std::uint64_t filtered_spatial_delivery_count = 0;
            for (const auto& relevance : current_commands_.replication_relevance_reports) {
                spatial_event_count += relevance.spatial_event_count;
                relevant_spatial_delivery_count +=
                    relevance.relevant_spatial_event_delivery_count;
                filtered_spatial_delivery_count +=
                    relevance.filtered_spatial_event_delivery_count;
            }
            HEARTSTEAD_PROFILE_PLOT("network.spatial_events", spatial_event_count);
            HEARTSTEAD_PROFILE_PLOT("network.spatial_event_deliveries",
                                    relevant_spatial_delivery_count);
            HEARTSTEAD_PROFILE_PLOT("network.spatial_event_filtered_deliveries",
                                    filtered_spatial_delivery_count);
            process_movement_control_messages(current_commands_.control_messages);
            for (const auto client_id : current_commands_.connected_clients) {
                auto connection_status = spawn_player(client_id);
                if (!connection_status) {
                    (void)host_.disconnect_client(client_id);
                    return connection_status;
                }
            }
            for (const auto client_id : current_commands_.disconnected_clients) {
                auto connection_status = remove_player_connection(client_id);
                if (!connection_status) {
                    return connection_status;
                }
            }
            bool collision_world_changed = false;
            for (const auto& report : current_commands_.command_reports) {
                if (!report.success) {
                    continue;
                }
                for (const auto& event : report.events) {
                    if (event.type != world::voxel_changed_event_type) {
                        continue;
                    }
                    auto change = world::VoxelChangeTextCodec::decode(event.message);
                    if (!change) {
                        return core::Status::failure(change.error().code, change.error().message);
                    }
                    collision_world_changed =
                        collision_world_changed ||
                        !collision_geometry_matches(change.value().previous, change.value().current,
                                                    *desc_.voxel_palette);
                    if (context.events != nullptr) {
                        auto event_status = context.events->voxel_changed.append(
                            {change.value().position, change.value().previous,
                             change.value().current});
                        if (!event_status) {
                            return event_status;
                        }
                    }
                }
            }
            if (collision_world_changed) {
                if (collision_world_revision_ == std::numeric_limits<std::uint64_t>::max()) {
                    return core::Status::failure(
                        "server_runtime.collision_revision_exhausted",
                        "authoritative collision-world revision space is exhausted");
                }
                ++collision_world_revision_;
            }
            return core::Status::ok();
        },
    });
    if (!status) {
        return status;
    }
    std::vector<std::string> chunk_collision_dependencies{"runtime.command_gateway"};
    if (renderer_proof_streaming_enabled_) {
        status = scheduler_.register_system({
            "runtime.renderer_proof_streaming",
            simulation::SimulationPhase::movement,
            {"runtime.command_gateway"},
            [this](simulation::SimulationContext&) { return stream_chunks(); },
        });
        if (!status) {
            return status;
        }
        chunk_collision_dependencies = {"runtime.renderer_proof_streaming"};
    }
    status = scheduler_.register_system({
        "runtime.chunk_collision",
        simulation::SimulationPhase::movement,
        std::move(chunk_collision_dependencies),
        [this](simulation::SimulationContext&) {
            // Renderer Proof exercises streaming and presentation throughput. Its authoritative
            // character path uses exact voxel collision, so cooking hundreds of duplicate Jolt
            // terrain compounds would only contaminate the renderer benchmark.
            if (renderer_proof_streaming_enabled_) {
                return core::Status::ok();
            }
            return chunk_collision_->update(world_.chunks(), world_.dirty_regions(),
                                            *desc_.voxel_palette);
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.chunk_fluids",
        simulation::SimulationPhase::movement,
        {"runtime.chunk_collision"},
        [this](simulation::SimulationContext& context) {
            if (renderer_proof_streaming_enabled_) {
                return core::Status::ok();
            }
            return chunk_fluids_->update(world_.chunks(), world_.dirty_regions(),
                                         *desc_.voxel_palette, context.tick);
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.character_movement",
        simulation::SimulationPhase::movement,
        {"runtime.chunk_fluids"},
        [this](simulation::SimulationContext& context) { return simulate_players(context); },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.physical_resources_prepare",
        simulation::SimulationPhase::movement,
        {"runtime.character_movement"},
        [this](simulation::SimulationContext& context) {
            return physical_resource_physics_->prepare(
                world_, *desc_.voxel_palette, static_cast<float>(context.fixed_delta_seconds));
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.physics",
        simulation::SimulationPhase::physics,
        {"runtime.physical_resources_prepare"},
        [this](simulation::SimulationContext& context) {
            auto result = physics_->step(
                physics::PhysicsStepDesc{static_cast<float>(context.fixed_delta_seconds)});
            if (!result) {
                return core::Status::failure(result.error().code, result.error().message);
            }
            current_physics_ = result.value();
            return core::Status::ok();
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.physical_resources_sync",
        simulation::SimulationPhase::environment,
        {"runtime.physics"},
        [this](simulation::SimulationContext&) {
            return physical_resource_physics_->synchronize(world_);
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.chunk_lighting",
        simulation::SimulationPhase::environment,
        {"runtime.physical_resources_sync"},
        [this](simulation::SimulationContext&) {
            // Proof terrain is static, contains no fluid or fire emitters, and is generated with
            // its final light values. Running dynamic world solvers would distort render timings.
            if (renderer_proof_streaming_enabled_) {
                return core::Status::ok();
            }
            auto sources = collect_fire_light_sources(world_, *desc_.prototypes);
            if (!sources) {
                return core::Status::failure(sources.error().code, sources.error().message);
            }
            return chunk_lighting_->update(world_.chunks(), world_.dirty_regions(),
                                           *desc_.voxel_palette, sources.value());
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.world_clock",
        simulation::SimulationPhase::environment,
        {"runtime.chunk_lighting"},
        [this](simulation::SimulationContext&) {
            pending_world_time_numerator_ += desc_.world_time.ticks_per_second;
            const auto elapsed_world_ticks =
                pending_world_time_numerator_ / desc_.simulation_ticks_per_second;
            pending_world_time_numerator_ %= desc_.simulation_ticks_per_second;
            return world_.advance_world_time(elapsed_world_ticks);
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.entity_finalize",
        simulation::SimulationPhase::finalize,
        {"runtime.world_clock"},
        [this](simulation::SimulationContext& context) {
            auto result = entities_.finalize_destruction(context.tick, context.events);
            return result ? core::Status::ok()
                          : core::Status::failure(result.error().code, result.error().message);
        },
    });
    if (!status) {
        return status;
    }
    status = scheduler_.register_system({
        "runtime.replication",
        simulation::SimulationPhase::replication,
        {"runtime.entity_finalize"},
        [this](simulation::SimulationContext& context) {
            const auto deltas =
                world::materialize_replication_deltas_for_tick(world_, current_commands_);
            auto delivery = world::send_replication_delta_snapshots_for_tick(
                host_, deltas, current_commands_, current_time_ms_);
            if (!delivery) {
                return core::Status::failure(delivery.error().code, delivery.error().message);
            }
            current_replication_ = std::move(delivery).value();
            auto chunk_status = synchronize_chunk_subscriptions();
            if (!chunk_status) {
                return chunk_status;
            }
            auto reliable_status = host_.flush_outbound(current_commands_);
            if (!reliable_status) {
                return reliable_status;
            }
            const auto players_first = (transient_replication_class_cursor_++ & 1U) == 0;
            auto transient_status = core::Status::ok();
            if (players_first) {
                transient_status = replicate_players();
                if (transient_status) {
                    transient_status = replicate_entity_motion(context.tick);
                }
            } else {
                transient_status = replicate_entity_motion(context.tick);
                if (transient_status) {
                    transient_status = replicate_players();
                }
            }
            if (!transient_status) {
                return transient_status;
            }
            // Reliable deltas, chunk slices, bootstrap state, and tombstones can be produced after
            // the command gateway. Drain them with the same tick counters so they neither bypass
            // quotas nor incur an accidental extra-tick latency when capacity remains.
            return host_.flush_outbound(current_commands_);
        },
    });
    if (!status) {
        return status;
    }
    status = gameplay_modules_.add(std::make_shared<interaction::VoxelInteractionModule>(
        [this](core::NetId client_id) { return player_for_client(client_id); },
        [this](world::BlockCoord position, world::VoxelCell cell) {
            return validate_voxel_placement(position, cell);
        }));
    if (!status) {
        return status;
    }
    for (auto& module : desc_.gameplay_modules) {
        status = gameplay_modules_.add(std::move(module));
        if (!status) {
            return status;
        }
    }
    GameplayRegistrationContext registration_context{*desc_.prototypes,
                                                     entities_,
                                                     commands_,
                                                     scheduler_,
                                                     component_registry_,
                                                     serialization_registry_,
                                                     persistence_registry_,
                                                     replication_registry_,
                                                     presentation_registry_,
                                                     domain_services_};
    auto registered = gameplay_modules_.register_all(registration_context);
    if (!registered) {
        return core::Status::failure(registered.error().code, registered.error().message);
    }
    if (desc_.initial_snapshot.has_value()) {
        status = persistence_registry_.restore_all(*desc_.initial_snapshot, world_);
        if (!status) {
            return status;
        }
    }
    return scheduler_.finalize();
}

core::Status ServerRuntime::start() {
    return host_.start();
}

core::Status ServerRuntime::stop() {
    auto status = host_.is_running() ? host_.stop() : core::Status::ok();
    if (status && chunk_loader_ != nullptr) {
        chunk_loader_->shutdown();
    }
    return status;
}

core::Result<ServerRuntimeTickStats>
ServerRuntime::run_tick(std::uint64_t tick, double fixed_delta_seconds, std::int64_t now_ms) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("server.tick");
    HEARTSTEAD_PROFILE_ZONE_VALUE(tick);
    if (!is_running()) {
        return core::Result<ServerRuntimeTickStats>::failure(
            "server_runtime.not_running", "server runtime must be started before ticking");
    }
    events_.clear();
    current_time_ms_ = now_ms;
    current_commands_ = {};
    current_replication_ = {};
    current_physics_ = {};
    current_moved_player_count_ = 0;
    current_repeated_input_count_ = 0;
    current_movement_event_count_ = 0;
    current_accepted_movement_input_count_ = 0;
    current_rejected_movement_input_count_ = 0;
    current_movement_snapshot_count_ = 0;
    current_entity_motion_snapshot_count_ = 0;
    current_entity_motion_tombstone_count_ = 0;
    current_player_tombstone_count_ = 0;
    current_chunk_subscriptions_ = {};
    current_chunk_streaming_ = {};
    current_chunk_streaming_.enabled = chunk_loader_ != nullptr;
    // Route spatial operation events against the exact chunk versions published before command
    // execution. A command may advance the authoritative revision later in this tick, but its
    // delta remains valid for clients that held the immediately preceding revision.
    refresh_replication_chunk_interest();
    auto budget_status = transient_replication_budget_.begin_tick(tick);
    if (!budget_status) {
        return core::Result<ServerRuntimeTickStats>::failure(budget_status.error().code,
                                                             budget_status.error().message);
    }
    auto simulation =
        scheduler_.run_tick({tick, fixed_delta_seconds, &world_, physics_.get(), &events_});
    if (!simulation) {
        return core::Result<ServerRuntimeTickStats>::failure(simulation.error().code,
                                                             simulation.error().message);
    }
    auto reliable_delivery_status =
        net::validate_host_session_outbound_delivery_report(current_commands_.outbound_delivery);
    if (!reliable_delivery_status) {
        return core::Result<ServerRuntimeTickStats>::failure(
            reliable_delivery_status.error().code, reliable_delivery_status.error().message);
    }
    ServerRuntimeTickStats stats;
    stats.simulation = simulation.value();
    stats.commands = current_commands_;
    stats.replication = current_replication_;
    stats.physics = current_physics_;
    stats.chunk_collision = chunk_collision_->stats();
    stats.physical_resources = physical_resource_physics_->stats();
    stats.chunk_fluids = chunk_fluids_->stats();
    stats.chunk_lighting = chunk_lighting_->stats();
    if (chunk_loader_ != nullptr) {
        stats.chunk_loading = chunk_loader_->stats();
    }
    stats.chunk_streaming = current_chunk_streaming_;
    stats.moved_player_count = current_moved_player_count_;
    stats.repeated_input_count = current_repeated_input_count_;
    stats.movement_event_count = current_movement_event_count_;
    stats.accepted_movement_input_count = current_accepted_movement_input_count_;
    stats.rejected_movement_input_count = current_rejected_movement_input_count_;
    stats.movement_snapshot_count = current_movement_snapshot_count_;
    stats.entity_motion_snapshot_count = current_entity_motion_snapshot_count_;
    stats.entity_motion_tombstone_count = current_entity_motion_tombstone_count_;
    stats.player_tombstone_count = current_player_tombstone_count_;
    stats.transient_replication = transient_replication_budget_.snapshot();
    stats.chunk_subscriptions = current_chunk_subscriptions_;
    auto replication_stats_status =
        net::validate_replication_tick_budget_stats(stats.transient_replication);
    if (!replication_stats_status) {
        return core::Result<ServerRuntimeTickStats>::failure(
            replication_stats_status.error().code, replication_stats_status.error().message);
    }
    stats.deferred_transient_snapshot_count = stats.transient_replication.deferred_message_count;
    stats.transient_snapshot_payload_bytes = stats.transient_replication.admitted_payload_bytes;
    return core::Result<ServerRuntimeTickStats>::success(std::move(stats));
}

core::Result<core::NetId> ServerRuntime::connect_client() {
    auto connected = host_.connect_client();
    if (!connected) {
        return connected;
    }
    auto status = spawn_player(connected.value());
    if (!status) {
        (void)host_.disconnect_client(connected.value());
        return core::Result<core::NetId>::failure(status.error().code, status.error().message);
    }
    std::map<world::ChunkCoord, EncodedChunkSnapshot> snapshot_cache;
    status =
        synchronize_client_chunk_subscription(connected.value(),
                                              {desc_.chunk_subscriptions.max_chunks_per_client,
                                               desc_.chunk_subscriptions.max_chunks_per_client},
                                              snapshot_cache, false);
    if (!status) {
        (void)disconnect_client(connected.value());
        return core::Result<core::NetId>::failure(status.error().code, status.error().message);
    }
    refresh_replication_chunk_interest();
    const auto connection = player_connections_.find(connected.value().value());
    if (connection != player_connections_.end() && initial_chunk_state_ready(connection->second)) {
        status = send_initial_state(connected.value());
        if (!status) {
            (void)disconnect_client(connected.value());
            return core::Result<core::NetId>::failure(status.error().code, status.error().message);
        }
    }
    net::HostSessionOutboundDeliveryReport bootstrap_delivery;
    status = host_.flush_local_client_bootstrap(connected.value(), bootstrap_delivery);
    if (!status) {
        (void)disconnect_client(connected.value());
        return core::Result<core::NetId>::failure(status.error().code, status.error().message);
    }
    status = net::validate_host_session_outbound_delivery_report(bootstrap_delivery);
    if (!status) {
        (void)disconnect_client(connected.value());
        return core::Result<core::NetId>::failure(status.error().code, status.error().message);
    }
    return connected;
}

core::Status ServerRuntime::disconnect_client(core::NetId client_id) {
    auto status = host_.disconnect_client(client_id);
    if (!status) {
        return status;
    }
    return remove_player_connection(client_id);
}

core::Status ServerRuntime::remove_player_connection(core::NetId client_id) {
    const auto found = player_connections_.find(client_id.value());
    if (found == player_connections_.end()) {
        return core::Status::ok();
    }
    const auto runtime_handle = found->second.runtime_handle;
    const auto entity_id = found->second.entity_id;
    const auto* player = players_.find(runtime_handle);
    const auto removed_player_net_id = player == nullptr ? core::NetId{} : player->net_id;
    const auto removed_player_save_id = player == nullptr ? core::SaveId{} : player->save_id;
    player_connections_.erase(found);
    auto policy = host_.replication_relevance_policy();
    std::erase_if(policy.chunk_interest_rules, [client_id](const auto& rule) {
        return rule.client_id == client_id;
    });
    host_.set_replication_relevance_policy(std::move(policy));
    if (removed_player_save_id.is_valid()) {
        revoke_private_subject_access(host_, client_id, removed_player_save_id);
    }
    (void)players_.erase(runtime_handle);
    const auto* legacy = world_.entities().find(runtime_handle);
    if (legacy != nullptr && !legacy->persistent) {
        (void)world_.entities().erase(runtime_handle);
    }
    if (entities_.is_alive(entity_id)) {
        auto status = entities_.destroy_entity(entity_id);
        if (!status) {
            return status;
        }
    }
    if (removed_player_net_id.is_valid()) {
        pending_player_removals_.push_back(removed_player_net_id);
    }
    return core::Status::ok();
}

core::Status ServerRuntime::submit_command(core::NetId client_id, net::CommandEnvelope command) {
    return host_.send_client_command(client_id, std::move(command));
}

core::Status ServerRuntime::submit_movement_input(core::NetId client_id,
                                                  movement::PlayerInputBundle bundle,
                                                  std::int64_t now_ms) {
    auto status = bundle.validate();
    if (!status) {
        return status;
    }
    return host_.send_client_control(client_id,
                                     movement::make_movement_input_bundle_message(bundle, now_ms));
}

core::Result<std::vector<net::TransportEnvelope>>
ServerRuntime::drain_client_messages(core::NetId client_id) {
    return host_.drain_client_messages(client_id);
}

bool ServerRuntime::is_running() const noexcept {
    return host_.is_running();
}

world::WorldState& ServerRuntime::world() noexcept {
    return world_;
}

const world::WorldState& ServerRuntime::world() const noexcept {
    return world_;
}

const world::VoxelPalette& ServerRuntime::voxel_palette() const noexcept {
    return *desc_.voxel_palette;
}

std::uint32_t ServerRuntime::physics_body_count() const noexcept {
    return physics_ == nullptr ? 0U : physics_->body_count();
}

entities::EntityWorld& ServerRuntime::entities() noexcept {
    return entities_;
}

const entities::EntityWorld& ServerRuntime::entities() const noexcept {
    return entities_;
}

net::HostSession& ServerRuntime::host() noexcept {
    return host_;
}

const net::HostSession& ServerRuntime::host() const noexcept {
    return host_;
}

const simulation::SimulationScheduler& ServerRuntime::scheduler() const noexcept {
    return scheduler_;
}

physics::ChunkCollisionSystem& ServerRuntime::chunk_collision() noexcept {
    return *chunk_collision_;
}

const physics::ChunkCollisionSystem& ServerRuntime::chunk_collision() const noexcept {
    return *chunk_collision_;
}

physics::PhysicalResourcePhysicsSystem& ServerRuntime::physical_resource_physics() noexcept {
    return *physical_resource_physics_;
}

const physics::PhysicalResourcePhysicsSystem&
ServerRuntime::physical_resource_physics() const noexcept {
    return *physical_resource_physics_;
}

world::ChunkFluidSystem& ServerRuntime::chunk_fluids() noexcept {
    return *chunk_fluids_;
}

const world::ChunkFluidSystem& ServerRuntime::chunk_fluids() const noexcept {
    return *chunk_fluids_;
}

world::ChunkLightSystem& ServerRuntime::chunk_lighting() noexcept {
    return *chunk_lighting_;
}

const world::ChunkLightSystem& ServerRuntime::chunk_lighting() const noexcept {
    return *chunk_lighting_;
}

const world::ChunkLoadSchedulerStats* ServerRuntime::chunk_loading_stats() const noexcept {
    return chunk_loader_ == nullptr ? nullptr : &chunk_loader_->stats();
}

core::Status ServerRuntime::drop_physical_resource(entities::PhysicalResourceRecord resource,
                                                   physics::Vec3 linear_velocity,
                                                   physics::Vec3 angular_velocity) {
    if (world_.contains_saved_object(resource.resource_id)) {
        return core::Status::failure(
            "server_runtime.duplicate_physical_resource",
            "dropped physical resource save id already belongs to a world object");
    }
    auto status = physical_resource_physics_->activate(resource, linear_velocity, angular_velocity);
    if (!status) {
        return status;
    }
    const auto body_id = resource.physics_body_id;
    status = world_.physical_resources().insert(std::move(resource));
    if (!status) {
        (void)physics_->destroy_body(body_id);
    }
    return status;
}

const simulation::TickEvents& ServerRuntime::events() const noexcept {
    return events_;
}

movement::PlayerControllerStore& ServerRuntime::players() noexcept {
    return players_;
}

const movement::PlayerControllerStore& ServerRuntime::players() const noexcept {
    return players_;
}

const GameplayModuleRegistry& ServerRuntime::gameplay_modules() const noexcept {
    return gameplay_modules_;
}

const ComponentRegistry& ServerRuntime::component_registry() const noexcept {
    return component_registry_;
}

const SerializationRegistry& ServerRuntime::serialization_registry() const noexcept {
    return serialization_registry_;
}

const PersistenceRegistry& ServerRuntime::persistence_registry() const noexcept {
    return persistence_registry_;
}

const ReplicationRegistry& ServerRuntime::replication_registry() const noexcept {
    return replication_registry_;
}

const PresentationRegistry& ServerRuntime::presentation_registry() const noexcept {
    return presentation_registry_;
}

DomainServiceRegistry& ServerRuntime::domain_services() noexcept {
    return domain_services_;
}

const DomainServiceRegistry& ServerRuntime::domain_services() const noexcept {
    return domain_services_;
}

movement::PlayerControllerRecord* ServerRuntime::player_for_client(core::NetId client_id) noexcept {
    const auto found = player_connections_.find(client_id.value());
    return found == player_connections_.end() ? nullptr
                                              : players_.find(found->second.runtime_handle);
}

const movement::PlayerControllerRecord*
ServerRuntime::player_for_client(core::NetId client_id) const noexcept {
    const auto found = player_connections_.find(client_id.value());
    return found == player_connections_.end() ? nullptr
                                              : players_.find(found->second.runtime_handle);
}

std::vector<ServerChunkSubscriptionClientSnapshot>
ServerRuntime::chunk_subscription_clients() const {
    std::vector<ServerChunkSubscriptionClientSnapshot> snapshots;
    snapshots.reserve(player_connections_.size());
    for (const auto& [client_id, connection] : player_connections_) {
        ServerChunkSubscriptionClientSnapshot snapshot;
        snapshot.client_id = core::NetId::from_value(client_id);
        snapshot.center = connection.chunk_subscription_center;
        snapshot.subscriptions = connection.chunk_subscriptions;
        for (const auto& [coordinate, publication] : connection.chunk_publications) {
            if (!publication.complete) {
                ++snapshot.partial_snapshot_count;
                continue;
            }
            const auto* chunk = world_.chunks().find(coordinate);
            if (chunk != nullptr && publication.identity == chunk->identity() &&
                publication.content_revision == chunk->content_revision()) {
                ++snapshot.published_chunk_count;
            } else {
                ++snapshot.stale_publication_count;
            }
        }
        snapshot.deferred_addition_count = connection.deferred_chunk_additions;
        snapshot.capacity_deferred_addition_count = connection.capacity_deferred_chunk_additions;
        snapshot.deferred_removal_count = connection.deferred_chunk_removals;
        snapshot.deferred_snapshot_count = connection.deferred_chunk_snapshots;
        snapshot.converged = connection.chunk_subscriptions_converged;
        snapshot.initial_state_published = connection.initial_state_published;
        snapshots.push_back(std::move(snapshot));
    }
    std::ranges::sort(snapshots, {},
                      [](const auto& snapshot) { return snapshot.client_id.value(); });
    return snapshots;
}

void ServerRuntime::process_movement_control_messages(
    std::span<const net::TransportEnvelope> messages) {
    for (const auto& message : messages) {
        const auto found = player_connections_.find(message.sender.value());
        if (found == player_connections_.end()) {
            ++current_rejected_movement_input_count_;
            continue;
        }
        if (message.message.payload_type == movement::movement_input_bundle_payload_type ||
            message.message.payload_type == movement::legacy_movement_input_bundle_payload_type) {
            auto bundle = movement::movement_input_bundle_from_transport(message);
            if (!bundle) {
                ++current_rejected_movement_input_count_;
                continue;
            }
            auto accepted = found->second.pending_inputs.push_bundle(bundle.value());
            if (!accepted) {
                ++current_rejected_movement_input_count_;
                continue;
            }
            current_accepted_movement_input_count_ += static_cast<std::uint32_t>(accepted.value());
        } else if (message.message.payload_type == movement::movement_input_payload_type) {
            auto input = movement::movement_input_from_transport(message);
            if (!input || !found->second.pending_inputs.push(std::move(input).value())) {
                ++current_rejected_movement_input_count_;
                continue;
            }
            ++current_accepted_movement_input_count_;
        } else {
            ++current_rejected_movement_input_count_;
        }
    }
}

core::Status ServerRuntime::ensure_spawn_area() {
    if (spawn_area_initialized_) {
        return core::Status::ok();
    }
    auto cancellation = startup_cancelled(desc_.stop_token);
    if (!cancellation) {
        return cancellation;
    }
    // A packaged fixture owns its complete chunk layout. Adding the foundation world at the
    // origin would both contaminate the fixture and place collision bodies outside a far-away
    // physics island.
    if (desc_.scenario.world_source != scenarios::ScenarioWorldSource::packaged_fixture) {
        auto built =
            foundation::build_world(world_.chunks(), *desc_.voxel_palette, desc_.stop_token);
        if (!built) {
            return core::Status::failure(built.error().code, built.error().message);
        }
    }
    if (!pending_saved_voxel_edits_.empty()) {
        auto status =
            world_.chunks().apply_saved_edits(pending_saved_voxel_edits_, world_.dirty_regions());
        if (!status) {
            return status;
        }
        pending_saved_voxel_edits_.clear();
    }
    spawn_area_initialized_ = true;
    return core::Status::ok();
}

core::Status ServerRuntime::validate_voxel_placement(world::BlockCoord position,
                                                     world::VoxelCell cell) const {
    const auto* definition = desc_.voxel_palette->find_by_type(cell.type);
    if (definition == nullptr) {
        return core::Status::failure(
            "voxel_command.unknown_placement_type",
            "voxel placement collision validation requires a known voxel type");
    }
    for (const auto* player : players_.records()) {
        const auto shape = player_controller_.shape_for(player->state);
        const auto feet = player->state.position.relative_to(position);
        for (const auto& source : definition->collision_bounds) {
            const math::Bounds3d voxel_bounds{
                {static_cast<double>(source.min.x), static_cast<double>(source.min.y),
                 static_cast<double>(source.min.z)},
                {static_cast<double>(source.max.x), static_cast<double>(source.max.y),
                 static_cast<double>(source.max.z)},
            };
            if (capsule_overlaps_bounds(feet, shape, voxel_bounds)) {
                return core::Status::failure(
                    "voxel_command.intersects_player",
                    "voxel placement would intersect an authoritative player capsule");
            }
        }
    }
    return core::Status::ok();
}

world::WorldPosition ServerRuntime::scenario_spawn_position() const noexcept {
    if (desc_.scenario.spawn_position.has_value()) {
        return *desc_.scenario.spawn_position;
    }
    switch (desc_.scenario.spawn_mode) {
    case scenarios::ScenarioSpawnMode::homestead:
        return foundation::spawn_position();
    case scenarios::ScenarioSpawnMode::outpost:
        return {16.5, 1.0, 16.5};
    case scenarios::ScenarioSpawnMode::debug:
        return {1.5, 1.0, 1.5};
    }
    return {8.5, 1.0, 8.5};
}

core::Status ServerRuntime::initialize_new_world_scenario() {
    auto status =
        world_.mod_states().insert({"engine", "scenario.id", desc_.scenario.prototype_id.value()});
    if (!status) {
        return status;
    }
    status = world_.mod_states().insert(
        {"engine", "scenario.start_region", desc_.scenario.start_region});
    if (!status) {
        return status;
    }
    status = world_.mod_states().insert(
        {"engine", "scenario.spawn_mode",
         std::string(scenarios::scenario_spawn_mode_name(desc_.scenario.spawn_mode))});
    if (!status) {
        return status;
    }
    if (desc_.scenario.prototype_id.value() == foundation::scenario_id) {
        status = world_.mod_states().insert({std::string(foundation::layout_state_mod),
                                             std::string(foundation::layout_state_key),
                                             std::to_string(foundation::layout_version)});
        if (!status) {
            return status;
        }
    }

    auto spawn = scenario_spawn_position();
    std::size_t cargo_offset = 0;
    for (const auto& cargo_id : desc_.scenario.starting_cargo) {
        auto cancellation = startup_cancelled(desc_.stop_token);
        if (!cancellation) {
            return cancellation;
        }
        const auto* prototype = desc_.prototypes->find(cargo_id);
        if (prototype == nullptr) {
            return core::Status::failure("server_runtime.starting_cargo_missing",
                                         "scenario starting cargo prototype is not loaded: " +
                                             cargo_id.value());
        }
        auto definition = cargo::cargo_definition_from_prototype(*prototype);
        if (!definition) {
            return core::Status::failure(definition.error().code, definition.error().message);
        }
        auto save_id = world_.save_ids().reserve();
        if (!save_id) {
            return core::Status::failure(save_id.error().code, save_id.error().message);
        }
        auto position = spawn;
        position.anchor.x += 2 + static_cast<std::int64_t>(cargo_offset);
        auto record = definition.value().create_record(save_id.value(), position);
        if (!record) {
            return core::Status::failure(record.error().code, record.error().message);
        }
        status = world_.cargo().insert(std::move(record).value());
        if (!status) {
            return status;
        }
        ++cargo_offset;
    }
    for (const auto& placement : desc_.scenario.scene_entities) {
        auto cancellation = startup_cancelled(desc_.stop_token);
        if (!cancellation) {
            return cancellation;
        }
        const auto* prototype = desc_.prototypes->find(placement.prototype_id);
        if (prototype == nullptr) {
            return core::Status::failure("server_runtime.scene_entity_missing",
                                         "scenario scene entity prototype is not loaded: " +
                                             placement.prototype_id.value());
        }
        auto definition = entities::entity_definition_from_prototype(*prototype);
        if (!definition) {
            return core::Status::failure(definition.error().code, definition.error().message);
        }
        auto runtime_handle = world_.runtime_handles().reserve();
        auto net_id = world_.entity_net_ids().reserve();
        if (!runtime_handle || !net_id) {
            const auto& error = !runtime_handle ? runtime_handle.error() : net_id.error();
            return core::Status::failure(error.code, error.message);
        }
        core::SaveId save_id;
        if (definition.value().persistent) {
            auto allocated_save_id = world_.save_ids().reserve();
            if (!allocated_save_id) {
                return core::Status::failure(allocated_save_id.error().code,
                                             allocated_save_id.error().message);
            }
            save_id = allocated_save_id.value();
        }
        auto record = definition.value().create_record(runtime_handle.value(), net_id.value(),
                                                       save_id, placement.transform);
        if (!record) {
            return core::Status::failure(record.error().code, record.error().message);
        }
        status = world_.entities().insert(std::move(record).value());
        if (!status) {
            return status;
        }
    }
    status = ensure_spawn_area();
    if (!status) {
        return status;
    }
    if (!desc_.scenario.setup_hook.empty()) {
        status = default_scenario_setup_registry().apply(desc_.scenario.setup_hook, world_,
                                                         *desc_.voxel_palette);
        if (!status) {
            return status;
        }
    }
    if (desc_.scenario.initial_world_time != 0) {
        status = world_.advance_world_time(desc_.scenario.initial_world_time);
        if (!status) {
            return status;
        }
    }
    return world_.mod_states().insert(
        {"engine", "scenario.weather", desc_.scenario.initial_weather});
}

core::Status ServerRuntime::grant_starting_inventory(core::SaveId owner_id) {
    if (world_.inventories().find(owner_id) != nullptr) {
        return core::Status::ok();
    }
    std::vector<items::ItemStack> stacks;
    stacks.reserve(desc_.scenario.starting_items.size());
    for (const auto& item_id : desc_.scenario.starting_items) {
        auto cancellation = startup_cancelled(desc_.stop_token);
        if (!cancellation) {
            return cancellation;
        }
        const auto* prototype = desc_.prototypes->find(item_id);
        if (prototype == nullptr) {
            return core::Status::failure("server_runtime.starting_item_missing",
                                         "scenario starting item prototype is not loaded: " +
                                             item_id.value());
        }
        auto definition = items::item_definition_from_prototype(*prototype);
        if (!definition) {
            return core::Status::failure(definition.error().code, definition.error().message);
        }
        auto stack = definition.value().create_stack(1);
        if (!stack) {
            return core::Status::failure(stack.error().code, stack.error().message);
        }
        stacks.push_back(std::move(stack).value());
    }
    return world_.inventories().insert({owner_id, std::move(stacks)});
}

core::Status ServerRuntime::spawn_player(core::NetId client_id) {
    if (!client_id.is_valid() || player_connections_.contains(client_id.value())) {
        return core::Status::failure("server_runtime.invalid_player_connection",
                                     "player connection is invalid or already exists");
    }
    auto status = ensure_spawn_area();
    if (!status) {
        return status;
    }
    const auto player_id = core::PrototypeId::parse("base:entities/player");
    const auto* prototype = player_id.has_value() ? desc_.prototypes->find(*player_id) : nullptr;
    if (prototype == nullptr) {
        return core::Status::failure("server_runtime.player_prototype_missing",
                                     "base player prototype is not registered");
    }
    auto definition = entities::entity_definition_from_prototype(*prototype);
    if (!definition) {
        return core::Status::failure(definition.error().code, definition.error().message);
    }
    const entities::EntityRecord* saved_player = nullptr;
    for (const auto* candidate : world_.entities().records()) {
        if (candidate->kind == entities::EntityKind::player &&
            players_.find(candidate->runtime_handle) == nullptr) {
            saved_player = candidate;
            break;
        }
    }
    core::RuntimeHandle runtime_handle;
    core::NetId net_id;
    core::SaveId save_id;
    world::WorldTransform transform;
    bool inserted_legacy_record = false;
    if (saved_player != nullptr) {
        runtime_handle = saved_player->runtime_handle;
        net_id = saved_player->net_id;
        save_id = saved_player->save_id;
        transform = saved_player->transform;
    } else {
        auto allocated_runtime = world_.runtime_handles().reserve();
        auto allocated_net_id = world_.entity_net_ids().reserve();
        auto allocated_save_id = world_.save_ids().reserve();
        if (!allocated_runtime || !allocated_net_id || !allocated_save_id) {
            const auto& error = !allocated_runtime  ? allocated_runtime.error()
                                : !allocated_net_id ? allocated_net_id.error()
                                                    : allocated_save_id.error();
            return core::Status::failure(error.code, error.message);
        }
        runtime_handle = allocated_runtime.value();
        net_id = allocated_net_id.value();
        save_id = allocated_save_id.value();
        transform.position = scenario_spawn_position();
        auto legacy_record =
            definition.value().create_record(runtime_handle, net_id, save_id, transform);
        if (!legacy_record) {
            return core::Status::failure(legacy_record.error().code, legacy_record.error().message);
        }
        status = world_.entities().insert(std::move(legacy_record).value());
        if (!status) {
            return status;
        }
        inserted_legacy_record = true;
    }

    auto entity_id = entities_.create_entity(*player_id);
    if (!entity_id) {
        if (inserted_legacy_record) {
            (void)world_.entities().erase(runtime_handle);
        }
        return core::Status::failure(entity_id.error().code, entity_id.error().message);
    }
    const auto cleanup_entity = [&]() {
        (void)entities_.destroy_entity(entity_id.value());
        (void)entities_.finalize_destruction(0, nullptr);
        if (inserted_legacy_record) {
            (void)world_.entities().erase(runtime_handle);
        }
    };
    auto transform_component = entities_.emplace<entities::TransformComponent>(
        entity_id.value(), entities::TransformComponent{transform, transform});
    if (!transform_component) {
        cleanup_entity();
        return core::Status::failure(transform_component.error().code,
                                     transform_component.error().message);
    }
    auto character = entities_.emplace<entities::CharacterComponent>(
        entity_id.value(), entities::CharacterComponent{client_id, 4.5F});
    if (!character) {
        cleanup_entity();
        return core::Status::failure(character.error().code, character.error().message);
    }
    status = entities_.activate_entity(entity_id.value());
    if (!status) {
        cleanup_entity();
        return status;
    }

    movement::PlayerControllerState controller_state;
    controller_state.position = transform.position;
    controller_state.fall_origin = transform.position;
    controller_state.scripted_start = transform.position;
    controller_state.scripted_target = transform.position;
    controller_state.mode = movement::PlayerControllerMode::grounded;
    controller_state.grounded = true;
    if (saved_player != nullptr &&
        saved_player->encoded_state.starts_with(movement::player_controller_save_state_magic)) {
        const auto payload = std::string_view(saved_player->encoded_state)
                                 .substr(movement::player_controller_save_state_magic.size());
        auto restored = movement::PlayerControllerSnapshotTextCodec::decode(
            payload, player_controller_.config());
        if (!restored) {
            cleanup_entity();
            return core::Status::failure(restored.error().code, restored.error().message);
        }
        controller_state = std::move(restored).value().state;
    }
    movement::PlayerControllerRecord controller_record;
    controller_record.runtime_handle = runtime_handle;
    controller_record.net_id = net_id;
    controller_record.save_id = save_id;
    controller_record.state = controller_state;
    controller_record.persistent = definition.value().persistent;
    std::unique_ptr<movement::PhysicsCharacterCollisionWorld> physics_collision;
    if (physics_->capabilities().supports_character_controllers &&
        !renderer_proof_streaming_enabled_) {
        movement::PhysicsCharacterCollisionConfig collision_config;
        collision_config.physics_island = desc_.chunk_collision.physics_island;
        collision_config.fixed_delta_seconds =
            1.0 / static_cast<double>(desc_.simulation_ticks_per_second);
        auto created_collision = movement::PhysicsCharacterCollisionWorld::create(
            *physics_, world_.chunks(), *desc_.voxel_palette, controller_state.position,
            player_controller_.shape_for(controller_state), collision_config);
        if (!created_collision) {
            cleanup_entity();
            return core::Status::failure(created_collision.error().code,
                                         created_collision.error().message);
        }
        physics_collision = std::move(created_collision).value();
    }
    status = players_.insert(std::move(controller_record), player_controller_.config());
    if (!status) {
        cleanup_entity();
        return status;
    }
    status = grant_starting_inventory(save_id);
    if (!status) {
        (void)players_.erase(runtime_handle);
        cleanup_entity();
        return status;
    }
    PlayerConnection connection;
    connection.runtime_handle = runtime_handle;
    connection.entity_id = entity_id.value();
    connection.physics_collision = std::move(physics_collision);
    player_connections_.emplace(client_id.value(), std::move(connection));
    grant_private_subject_access(host_, client_id, save_id);
    refresh_replication_chunk_interest();
    return core::Status::ok();
}

core::Status ServerRuntime::simulate_players(simulation::SimulationContext& context) {
    movement::VoxelCharacterCollisionWorld voxel_collision(world_.chunks(), *desc_.voxel_palette);
    std::vector<std::uint64_t> client_ids;
    client_ids.reserve(player_connections_.size());
    for (const auto& [client_id, _] : player_connections_) {
        client_ids.push_back(client_id);
    }
    std::ranges::sort(client_ids);

    for (const auto client_id : client_ids) {
        auto& connection = player_connections_.at(client_id);
        auto* player = players_.find(connection.runtime_handle);
        if (player == nullptr) {
            return core::Status::failure("server_runtime.player_record_missing",
                                         "connected player has no controller record");
        }
        auto pending = connection.pending_inputs.drain(1);
        movement::PlayerInputFrame input;
        if (!pending.empty()) {
            input = pending.front();
        } else if (connection.last_input.has_value()) {
            input = *connection.last_input;
            input.pressed_buttons = 0;
            ++current_repeated_input_count_;
        } else {
            continue;
        }
        input.tick = context.tick;
        const auto previous_position = player->state.position;
        movement::ICharacterCollisionWorld* collision = &voxel_collision;
        if (connection.physics_collision != nullptr) {
            auto status =
                connection.physics_collision->set_fixed_delta_seconds(context.fixed_delta_seconds);
            if (!status) {
                return status;
            }
            collision = connection.physics_collision.get();
        }
        auto ticked = player_controller_.tick(player->state, input, player->modifiers, *collision);
        if (!ticked) {
            return core::Status::failure(ticked.error().code, ticked.error().message);
        }
        connection.last_input = input;
        current_movement_event_count_ += static_cast<std::uint32_t>(ticked.value().events.size());
        player->state = std::move(ticked).value().state;

        if (auto* legacy = world_.entities().find(connection.runtime_handle); legacy != nullptr) {
            legacy->transform.position = player->state.position;
            legacy->transform.rotation_degrees = {
                0.0, static_cast<double>(player->state.yaw_centidegrees) * 0.01, 0.0};
        }
        auto* transform =
            entities_.find_component<entities::TransformComponent>(connection.entity_id);
        if (transform == nullptr) {
            return core::Status::failure("server_runtime.player_transform_missing",
                                         "connected player has no transform component");
        }
        transform->previous = transform->current;
        transform->current.position = player->state.position;
        transform->current.rotation_degrees = {
            0.0, static_cast<double>(player->state.yaw_centidegrees) * 0.01, 0.0};
        if (player->state.position != previous_position) {
            ++current_moved_player_count_;
            if (context.events != nullptr) {
                auto event_status = context.events->character_moved.append(
                    {connection.entity_id, previous_position, player->state.position});
                if (!event_status) {
                    return event_status;
                }
            }
        }
    }
    return core::Status::ok();
}

core::Status ServerRuntime::stream_chunks() {
    HEARTSTEAD_PROFILE_ZONE_NAMED("streaming.predictive_runtime");
    if (!renderer_proof_streaming_enabled_) {
        return core::Status::ok();
    }
    if (chunk_loader_ == nullptr) {
        return core::Status::failure("server_runtime.chunk_loader_missing",
                                     "renderer-proof streaming has no chunk-load scheduler");
    }
    if (current_time_ms_ < 0) {
        return core::Status::failure("server_runtime.invalid_streaming_time",
                                     "chunk streaming requires nonnegative runtime time");
    }

    auto players = players_.records();
    std::ranges::sort(
        players, [](const auto* left, const auto* right) { return left->net_id < right->net_id; });
    std::vector<world::ChunkStreamViewerMotion> viewers;
    viewers.reserve(players.size());
    std::map<std::uint64_t, world::ChunkCoord> current_viewer_chunks;
    constexpr double degrees_to_radians = 3.14159265358979323846 / 180.0;
    const auto teleport_threshold =
        std::max<std::uint64_t>(1, desc_.chunk_streaming.max_prediction_distance_chunks);
    const auto axis_distance = [](std::int64_t left, std::int64_t right) noexcept {
        const auto ordered = [](std::int64_t value) noexcept {
            return static_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << 63U);
        };
        const auto ordered_left = ordered(left);
        const auto ordered_right = ordered(right);
        return ordered_left >= ordered_right ? ordered_left - ordered_right
                                             : ordered_right - ordered_left;
    };
    for (const auto* player : players) {
        world::ChunkStreamViewerMotion motion;
        motion.viewer.viewer_id = player->net_id;
        motion.viewer.coord = player->state.position.anchor;
        motion.velocity_x_blocks_per_second = player->state.velocity.x;
        motion.velocity_y_blocks_per_second = player->state.velocity.y;
        motion.velocity_z_blocks_per_second = player->state.velocity.z;
        const auto yaw =
            static_cast<double>(player->state.yaw_centidegrees) * 0.01 * degrees_to_radians;
        const auto pitch =
            static_cast<double>(player->state.pitch_centidegrees) * 0.01 * degrees_to_radians;
        motion.view_direction_x = std::sin(yaw) * std::cos(pitch);
        motion.view_direction_y = std::sin(pitch);
        motion.view_direction_z = std::cos(yaw) * std::cos(pitch);

        const auto current_chunk = world::chunk_coord_for_block(player->state.position.anchor);
        if (const auto previous = previous_chunk_streaming_viewers_.find(player->net_id.value());
            previous != previous_chunk_streaming_viewers_.end()) {
            const auto dx = axis_distance(previous->second.x, current_chunk.x);
            const auto dy = axis_distance(previous->second.y, current_chunk.y);
            const auto dz = axis_distance(previous->second.z, current_chunk.z);
            motion.teleport = dx > teleport_threshold || dy > teleport_threshold ||
                              dz > teleport_threshold;
            if (!motion.teleport) {
                // The component guard above bounds every product to at most 32^2.
                motion.teleport = dx * dx + dy * dy + dz * dz >
                                  teleport_threshold * teleport_threshold;
            }
        }
        current_viewer_chunks.emplace(player->net_id.value(), current_chunk);
        viewers.push_back(motion);
    }

    auto updated =
        chunk_streaming_controller_.update(world_, *chunk_loader_, viewers, desc_.chunk_streaming,
                                           world::ChunkStreamMemoryPressure::nominal,
                                           static_cast<simulation::WorldTick>(current_time_ms_));
    if (!updated) {
        return core::Status::failure(updated.error().code, updated.error().message);
    }
    auto report = std::move(updated).value();
    previous_chunk_streaming_viewers_ = std::move(current_viewer_chunks);

    current_chunk_streaming_.enabled = true;
    current_chunk_streaming_.lifetime = chunk_streaming_controller_.stats();
    current_chunk_streaming_.desired_chunk_count = report.policy.immediate.desired_chunk_count;
    current_chunk_streaming_.target_resident_chunk_count =
        report.policy.target_resident_chunk_count;
    current_chunk_streaming_.submitted_required_count = report.submitted_required.size();
    current_chunk_streaming_.submitted_speculative_count = report.submitted_speculative.size();
    current_chunk_streaming_.explicit_speculative_cancellation_count =
        report.explicit_speculative_cancellations;
    current_chunk_streaming_.obsolete_cancellation_signal_count =
        report.obsolete_cancellation_signals;
    current_chunk_streaming_.deferred_required_load_count = report.deferred_required_loads;
    current_chunk_streaming_.evicted_chunk_count = report.eviction.evicted_count();
    current_chunk_streaming_.deferred_eviction_count = report.policy.deferred_eviction_count;
    current_chunk_streaming_.projected_resident_overage = report.policy.projected_resident_overage;
    current_chunk_streaming_.unresolved_resident_overage =
        report.policy.unresolved_resident_overage;
    current_chunk_streaming_.pending_load_count = report.pending_loads;
    current_chunk_streaming_.teleport_mode = report.policy.teleport_mode;
    HEARTSTEAD_PROFILE_PLOT("streaming.policy.desired",
                            current_chunk_streaming_.desired_chunk_count);
    HEARTSTEAD_PROFILE_PLOT("streaming.policy.pending",
                            current_chunk_streaming_.pending_load_count);
    HEARTSTEAD_PROFILE_PLOT("streaming.policy.active_speculative",
                            current_chunk_streaming_.lifetime.active_speculative_requests);
    HEARTSTEAD_PROFILE_PLOT("streaming.policy.deferred_evictions",
                            current_chunk_streaming_.deferred_eviction_count);
    HEARTSTEAD_PROFILE_PLOT("streaming.policy.projected_overage",
                            current_chunk_streaming_.projected_resident_overage);
    HEARTSTEAD_PROFILE_PLOT("streaming.policy.prediction_accuracy",
                            current_chunk_streaming_.lifetime.prediction_accuracy);

    for (const auto& published : report.publication.published) {
        if (collision_world_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            return core::Status::failure(
                "server_runtime.collision_revision_exhausted",
                "authoritative collision-world revision space is exhausted");
        }
        ++collision_world_revision_;
        pending_streamed_chunks_.push_back(published.coord);
    }
    for (std::size_t index = 0; index < report.eviction.evicted_count(); ++index) {
        if (collision_world_revision_ == std::numeric_limits<std::uint64_t>::max()) {
            return core::Status::failure(
                "server_runtime.collision_revision_exhausted",
                "authoritative collision-world revision space is exhausted");
        }
        ++collision_world_revision_;
    }
    return core::Status::ok();
}

core::Status ServerRuntime::replicate_players() {
    std::vector<std::uint64_t> recipients;
    recipients.reserve(player_connections_.size());
    for (const auto& [client_id, connection] : player_connections_) {
        if (connection.initial_state_published) {
            recipients.push_back(client_id);
        }
    }
    std::ranges::sort(recipients);
    if (!recipients.empty()) {
        const auto offset =
            static_cast<std::size_t>(transient_replication_recipient_cursor_ % recipients.size());
        const auto iterator_offset =
            static_cast<std::vector<std::uint64_t>::difference_type>(offset);
        std::rotate(recipients.begin(), recipients.begin() + iterator_offset, recipients.end());
        ++transient_replication_recipient_cursor_;
    }
    for (const auto recipient : recipients) {
        for (const auto removed_player : pending_player_removals_) {
            auto sequence = reserve_custom_replication_sequence();
            if (!sequence) {
                return core::Status::failure(sequence.error().code, sequence.error().message);
            }
            auto status = host_.send_replication_message(
                core::NetId::from_value(recipient),
                movement::make_player_removal_message(removed_player, sequence.value(),
                                                      current_time_ms_));
            if (!status) {
                return status;
            }
            ++current_player_tombstone_count_;
        }
    }
    if (!pending_player_removals_.empty()) {
        auto status = host_.flush_outbound(current_commands_);
        if (!status) {
            return status;
        }
    }

    std::vector<movement::PlayerControllerSnapshot> snapshots;
    snapshots.reserve(player_connections_.size());
    const auto collision_revision = collision_world_revision();
    for (const auto& [_, connection] : player_connections_) {
        const auto* player = players_.find(connection.runtime_handle);
        if (player == nullptr || player->state.simulation_tick == 0) {
            continue;
        }
        movement::PlayerControllerSnapshot snapshot;
        snapshot.player_net_id = player->net_id;
        snapshot.player_save_id = player->save_id;
        snapshot.state = player->state;
        snapshot.last_processed_input_sequence = player->state.last_input_sequence;
        snapshot.collision_world_revision = collision_revision;
        snapshots.push_back(std::move(snapshot));
    }
    std::ranges::sort(snapshots, {}, [](const movement::PlayerControllerSnapshot& snapshot) {
        return snapshot.player_net_id.value();
    });
    if (!snapshots.empty()) {
        const auto offset =
            static_cast<std::size_t>(movement_replication_source_cursor_ % snapshots.size());
        const auto iterator_offset =
            static_cast<std::vector<movement::PlayerControllerSnapshot>::difference_type>(offset);
        std::rotate(snapshots.begin(), snapshots.begin() + iterator_offset, snapshots.end());
        ++movement_replication_source_cursor_;
    }

    auto replication_status = replicate_transient_snapshot_candidates(
        transient_replication_budget_, recipients, snapshots,
        [](const movement::PlayerControllerSnapshot& snapshot) {
            return movement::PlayerControllerSnapshotBinaryCodec::encode(snapshot);
        },
        [this](core::NetId recipient, const std::string& payload) {
            auto sequence = reserve_custom_replication_sequence();
            if (!sequence) {
                return core::Status::failure(sequence.error().code, sequence.error().message);
            }
            auto status = host_.send_replication_message(
                recipient, movement::make_encoded_movement_snapshot_message(
                               payload, current_time_ms_, sequence.value()));
            if (!status) {
                return status;
            }
            ++current_movement_snapshot_count_;
            return core::Status::ok();
        });
    if (!replication_status) {
        return replication_status;
    }
    pending_player_removals_.clear();
    return core::Status::ok();
}

core::Status ServerRuntime::replicate_entity_motion(std::uint64_t simulation_tick) {
    if (player_connections_.empty()) {
        pending_entity_motion_removals_.clear();
        return core::Status::ok();
    }
    std::vector<entities::EntityMotionSnapshot> snapshots;
    for (const auto& record : entities_.records()) {
        if (record.lifecycle != entities::EntityLifecycle::active) {
            continue;
        }
        const auto* identity =
            entities_.find_component<entities::NetworkIdentityComponent>(record.id);
        const auto* transform = entities_.find_component<entities::TransformComponent>(record.id);
        const auto* locomotion =
            entities_.find_component<entities::LocomotionAnimationComponent>(record.id);
        if (identity == nullptr || transform == nullptr || locomotion == nullptr) {
            continue;
        }
        entities::EntityMotionSnapshot snapshot;
        snapshot.entity_net_id = identity->net_id;
        snapshot.prototype_id = record.prototype;
        snapshot.previous_transform = transform->previous;
        snapshot.current_transform = transform->current;
        snapshot.locomotion = locomotion->state;
        if (const auto* visual_state =
                entities_.find_component<entities::VisualStateComponent>(record.id);
            visual_state != nullptr) {
            snapshot.visual_states = visual_state->states;
        }
        snapshot.simulation_tick = simulation_tick;
        auto status = snapshot.validate();
        if (!status) {
            return status;
        }
        snapshots.push_back(std::move(snapshot));
    }
    std::ranges::sort(snapshots, {}, [](const entities::EntityMotionSnapshot& snapshot) {
        return snapshot.entity_net_id.value();
    });
    if (!snapshots.empty()) {
        const auto offset =
            static_cast<std::size_t>(entity_motion_replication_source_cursor_ % snapshots.size());
        const auto iterator_offset =
            static_cast<std::vector<entities::EntityMotionSnapshot>::difference_type>(offset);
        std::rotate(snapshots.begin(), snapshots.begin() + iterator_offset, snapshots.end());
        ++entity_motion_replication_source_cursor_;
    }
    std::vector<std::uint64_t> recipients;
    recipients.reserve(player_connections_.size());
    for (const auto& [client_id, connection] : player_connections_) {
        if (connection.initial_state_published) {
            recipients.push_back(client_id);
        }
    }
    std::ranges::sort(recipients);
    if (!recipients.empty()) {
        const auto offset =
            static_cast<std::size_t>(transient_replication_recipient_cursor_ % recipients.size());
        const auto iterator_offset =
            static_cast<std::vector<std::uint64_t>::difference_type>(offset);
        std::rotate(recipients.begin(), recipients.begin() + iterator_offset, recipients.end());
        ++transient_replication_recipient_cursor_;
    }
    for (const auto recipient : recipients) {
        for (const auto removed : pending_entity_motion_removals_) {
            auto sequence = reserve_custom_replication_sequence();
            if (!sequence) {
                return core::Status::failure(sequence.error().code, sequence.error().message);
            }
            auto status =
                host_.send_replication_message(core::NetId::from_value(recipient),
                                               entities::make_entity_motion_removal_message(
                                                   removed, sequence.value(), current_time_ms_));
            if (!status) {
                return status;
            }
            ++current_entity_motion_tombstone_count_;
        }
    }
    if (!pending_entity_motion_removals_.empty()) {
        auto status = host_.flush_outbound(current_commands_);
        if (!status) {
            return status;
        }
    }
    auto replication_status = replicate_transient_snapshot_candidates(
        transient_replication_budget_, recipients, snapshots,
        [](const entities::EntityMotionSnapshot& snapshot) {
            return entities::EntityMotionSnapshotTextCodec::encode(snapshot);
        },
        [this](core::NetId recipient, const std::string& payload) {
            auto sequence = reserve_custom_replication_sequence();
            if (!sequence) {
                return core::Status::failure(sequence.error().code, sequence.error().message);
            }
            auto status = host_.send_replication_message(
                recipient, entities::make_encoded_entity_motion_snapshot_message(
                               payload, sequence.value(), current_time_ms_));
            if (!status) {
                return status;
            }
            ++current_entity_motion_snapshot_count_;
            return core::Status::ok();
        });
    if (!replication_status) {
        return replication_status;
    }
    pending_entity_motion_removals_.clear();
    return core::Status::ok();
}

core::Status ServerRuntime::synchronize_chunk_subscriptions() {
    HEARTSTEAD_PROFILE_ZONE_NAMED("network.chunk_subscription_sync");
    const auto finalize_stats = [this]() {
        if (current_chunk_subscriptions_.snapshot_serialization_time_us >
            desc_.max_chunk_snapshot_serialization_time_us_per_tick) {
            current_chunk_subscriptions_.snapshot_serialization_time_overshoot_us =
                current_chunk_subscriptions_.snapshot_serialization_time_us -
                desc_.max_chunk_snapshot_serialization_time_us_per_tick;
        }
        current_chunk_subscriptions_.client_count =
            static_cast<std::uint32_t>(player_connections_.size());
        for (const auto& [_, connection] : player_connections_) {
            current_chunk_subscriptions_.subscription_count +=
                connection.chunk_subscriptions.size();
            current_chunk_subscriptions_.maximum_client_subscription_count =
                std::max(current_chunk_subscriptions_.maximum_client_subscription_count,
                         connection.chunk_subscriptions.size());
            for (const auto& [coordinate, publication] : connection.chunk_publications) {
                if (!publication.complete) {
                    ++current_chunk_subscriptions_.partial_snapshot_count;
                    continue;
                }
                const auto* chunk = world_.chunks().find(coordinate);
                if (chunk != nullptr && publication.identity == chunk->identity() &&
                    publication.content_revision == chunk->content_revision()) {
                    ++current_chunk_subscriptions_.published_chunk_count;
                } else {
                    ++current_chunk_subscriptions_.stale_publication_count;
                }
            }
            current_chunk_subscriptions_.converged_client_count +=
                connection.chunk_subscriptions_converged ? 1U : 0U;
            current_chunk_subscriptions_.pending_initial_state_client_count +=
                connection.initial_state_published ? 0U : 1U;
        }
        HEARTSTEAD_PROFILE_PLOT("network.chunk_subscriptions",
                                current_chunk_subscriptions_.subscription_count);
        HEARTSTEAD_PROFILE_PLOT("network.chunk_snapshot_slices",
                                current_chunk_subscriptions_.snapshot_slice_message_count);
        HEARTSTEAD_PROFILE_PLOT("network.chunk_snapshot_deferred",
                                current_chunk_subscriptions_.deferred_snapshot_count);
        refresh_replication_chunk_interest();
    };
    if (desc_.direct_local_chunk_replication && renderer_proof_streaming_enabled_) {
        // A local Renderer Proof session installs streamed chunks directly into its presentation
        // client. Encoding every chunk as 32 reliable transport messages would benchmark the
        // loopback protocol instead of the renderer and create large main-thread bursts.
        pending_streamed_chunks_.clear();
        finalize_stats();
        return core::Status::ok();
    }
    if (player_connections_.empty()) {
        pending_streamed_chunks_.clear();
        finalize_stats();
        return core::Status::ok();
    }
    std::vector<std::uint64_t> client_ids;
    client_ids.reserve(player_connections_.size());
    for (const auto& [client_id, _] : player_connections_) {
        client_ids.push_back(client_id);
    }
    std::ranges::sort(client_ids);
    const auto client_offset =
        static_cast<std::size_t>(chunk_subscription_client_cursor_++ % client_ids.size());
    std::rotate(client_ids.begin(),
                client_ids.begin() +
                    static_cast<std::vector<std::uint64_t>::difference_type>(client_offset),
                client_ids.end());
    std::map<world::ChunkCoord, EncodedChunkSnapshot> snapshot_cache;
    for (const auto client_id : client_ids) {
        auto status = synchronize_client_chunk_subscription(
            core::NetId::from_value(client_id),
            {desc_.chunk_subscriptions.max_additions_per_update,
             desc_.chunk_subscriptions.max_removals_per_update},
            snapshot_cache);
        if (!status) {
            return status;
        }
        auto& connection = player_connections_.at(client_id);
        if (!connection.initial_state_published && initial_chunk_state_ready(connection)) {
            status = send_initial_state(core::NetId::from_value(client_id));
            if (!status) {
                return status;
            }
        }
    }
    pending_streamed_chunks_.clear();
    finalize_stats();
    return core::Status::ok();
}

void ServerRuntime::refresh_replication_chunk_interest() {
    auto policy = host_.replication_relevance_policy();
    for (const auto& [client_id, _] : player_connections_) {
        std::erase_if(policy.chunk_interest_rules, [client_id](const auto& rule) {
            return rule.client_id.value() == client_id;
        });
    }

    policy.chunk_interest_rules.reserve(policy.chunk_interest_rules.size() +
                                        player_connections_.size());
    for (const auto& [client_id, connection] : player_connections_) {
        net::ReplicationChunkInterestRule rule;
        rule.client_id = core::NetId::from_value(client_id);
        rule.visible_chunks.reserve(connection.chunk_publications.size());
        for (const auto& [coordinate, publication] : connection.chunk_publications) {
            if (!publication.complete) {
                continue;
            }
            const auto* chunk = world_.chunks().find(coordinate);
            if (chunk != nullptr && publication.identity == chunk->identity() &&
                publication.content_revision == chunk->content_revision()) {
                rule.visible_chunks.push_back(coordinate);
            }
        }
        policy.chunk_interest_rules.push_back(std::move(rule));
    }
    std::ranges::sort(policy.chunk_interest_rules, {},
                      [](const auto& rule) { return rule.client_id.value(); });
    host_.set_replication_relevance_policy(std::move(policy));
}

core::Status ServerRuntime::synchronize_client_chunk_subscription(
    core::NetId client_id, world::ChunkSubscriptionTransitionBudget transition_budget,
    std::map<world::ChunkCoord, EncodedChunkSnapshot>& snapshot_cache,
    bool enforce_serialization_budget) {
    const auto found = player_connections_.find(client_id.value());
    if (found == player_connections_.end()) {
        return core::Status::failure("server_runtime.player_not_connected",
                                     "chunk subscription client has no active player");
    }
    auto& connection = found->second;
    const auto* player = players_.find(connection.runtime_handle);
    if (player == nullptr) {
        return core::Status::failure("server_runtime.player_record_missing",
                                     "chunk subscription client has no controller record");
    }
    connection.chunk_subscription_center =
        world::chunk_coord_for_block(player->state.position.anchor);
    auto planned = world::plan_chunk_subscriptions(connection.chunk_subscriptions,
                                                   connection.chunk_subscription_center,
                                                   desc_.chunk_subscriptions, transition_budget);
    if (!planned) {
        return core::Status::failure(planned.error().code, planned.error().message);
    }
    connection.deferred_chunk_additions = planned.value().deferred_addition_count;
    connection.capacity_deferred_chunk_additions = planned.value().capacity_deferred_addition_count;
    connection.deferred_chunk_removals = planned.value().deferred_removal_count;
    connection.deferred_chunk_snapshots = 0;
    current_chunk_subscriptions_.deferred_addition_count += planned.value().deferred_addition_count;
    current_chunk_subscriptions_.capacity_deferred_addition_count +=
        planned.value().capacity_deferred_addition_count;
    current_chunk_subscriptions_.deferred_removal_count += planned.value().deferred_removal_count;

    const auto erase_subscription = [&](world::ChunkCoord coordinate) {
        const auto position = std::ranges::lower_bound(connection.chunk_subscriptions, coordinate);
        if (position != connection.chunk_subscriptions.end() && *position == coordinate) {
            connection.chunk_subscriptions.erase(position);
            return true;
        }
        return false;
    };
    const auto queue_removal = [&](world::ChunkCoord coordinate) -> core::Result<bool> {
        auto sequence = reserve_custom_replication_sequence();
        if (!sequence) {
            return core::Result<bool>::failure(sequence.error().code, sequence.error().message);
        }
        auto status = host_.send_replication_message(
            client_id, world::make_chunk_subscription_removal_message(
                           {coordinate}, sequence.value(), current_time_ms_));
        if (!status) {
            if (net::is_host_session_reliable_backlog_capacity_error(status.error().code)) {
                ++current_chunk_subscriptions_.reliable_admission_deferral_count;
                return core::Result<bool>::success(false);
            }
            return core::Result<bool>::failure(status.error().code, status.error().message);
        }
        ++current_chunk_subscriptions_.removal_message_count;
        return core::Result<bool>::success(true);
    };

    for (const auto coordinate : planned.value().removed_chunks) {
        const auto publication = connection.chunk_publications.find(coordinate);
        if (publication != connection.chunk_publications.end()) {
            auto queued = queue_removal(coordinate);
            if (!queued) {
                return core::Status::failure(queued.error().code, queued.error().message);
            }
            if (!queued.value()) {
                ++connection.deferred_chunk_removals;
                ++current_chunk_subscriptions_.deferred_removal_count;
                continue;
            }
            connection.chunk_publications.erase(publication);
        }
        if (erase_subscription(coordinate)) {
            ++current_chunk_subscriptions_.removed_subscription_count;
        }
    }
    for (const auto coordinate : planned.value().added_chunks) {
        if (connection.chunk_subscriptions.size() >=
            desc_.chunk_subscriptions.max_chunks_per_client) {
            ++connection.deferred_chunk_additions;
            ++connection.capacity_deferred_chunk_additions;
            ++current_chunk_subscriptions_.deferred_addition_count;
            ++current_chunk_subscriptions_.capacity_deferred_addition_count;
            continue;
        }
        const auto position = std::ranges::lower_bound(connection.chunk_subscriptions, coordinate);
        if (position == connection.chunk_subscriptions.end() || *position != coordinate) {
            connection.chunk_subscriptions.insert(position, coordinate);
            ++current_chunk_subscriptions_.added_subscription_count;
        }
    }

    for (auto publication = connection.chunk_publications.begin();
         publication != connection.chunk_publications.end();) {
        if (world_.chunks().contains(publication->first)) {
            ++publication;
            continue;
        }
        auto queued = queue_removal(publication->first);
        if (!queued) {
            return core::Status::failure(queued.error().code, queued.error().message);
        }
        if (!queued.value()) {
            ++connection.deferred_chunk_snapshots;
            ++current_chunk_subscriptions_.deferred_snapshot_count;
            ++publication;
            continue;
        }
        publication = connection.chunk_publications.erase(publication);
    }

    std::vector<world::ChunkCoord> snapshot_candidates;
    snapshot_candidates.reserve(connection.chunk_subscriptions.size());
    for (const auto coordinate : connection.chunk_subscriptions) {
        const auto* chunk = world_.chunks().find(coordinate);
        if (chunk == nullptr) {
            continue;
        }
        const auto publication = connection.chunk_publications.find(coordinate);
        if (publication == connection.chunk_publications.end() || !publication->second.complete ||
            publication->second.identity != chunk->identity() ||
            publication->second.content_revision != chunk->content_revision()) {
            snapshot_candidates.push_back(coordinate);
        }
    }
    if (!snapshot_candidates.empty()) {
        const auto offset = connection.chunk_snapshot_cursor++ % snapshot_candidates.size();
        std::rotate(snapshot_candidates.begin(),
                    snapshot_candidates.begin() +
                        static_cast<std::vector<world::ChunkCoord>::difference_type>(offset),
                    snapshot_candidates.end());
    }

    constexpr auto messages_per_snapshot = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    for (std::size_t candidate_index = 0; candidate_index < snapshot_candidates.size();
         ++candidate_index) {
        const auto remaining_candidates = snapshot_candidates.size() - candidate_index;
        const auto client_pending = host_.pending_outbound_message_count(client_id);
        const auto global_pending = host_.pending_outbound_message_count();
        const auto client_message_capacity =
            client_pending <=
            desc_.host.max_pending_reliable_messages_per_client -
                std::min(
                    messages_per_snapshot,
                    static_cast<std::size_t>(desc_.host.max_pending_reliable_messages_per_client));
        const auto global_message_capacity =
            global_pending <=
            desc_.host.max_pending_reliable_messages -
                std::min(messages_per_snapshot,
                         static_cast<std::size_t>(desc_.host.max_pending_reliable_messages));
        if (desc_.host.max_pending_reliable_messages_per_client < messages_per_snapshot ||
            desc_.host.max_pending_reliable_messages < messages_per_snapshot ||
            !client_message_capacity || !global_message_capacity) {
            connection.deferred_chunk_snapshots += remaining_candidates;
            current_chunk_subscriptions_.deferred_snapshot_count += remaining_candidates;
            ++current_chunk_subscriptions_.reliable_admission_deferral_count;
            break;
        }

        const auto coordinate = snapshot_candidates[candidate_index];
        const auto* chunk = world_.chunks().find(coordinate);
        if (chunk == nullptr) {
            continue;
        }
        auto cached = snapshot_cache.find(coordinate);
        if (cached == snapshot_cache.end() || cached->second.identity != chunk->identity() ||
            cached->second.content_revision != chunk->content_revision()) {
            if (enforce_serialization_budget &&
                current_chunk_subscriptions_.snapshot_serialization_time_us >=
                desc_.max_chunk_snapshot_serialization_time_us_per_tick) {
                ++connection.deferred_chunk_snapshots;
                ++current_chunk_subscriptions_.deferred_snapshot_count;
                ++current_chunk_subscriptions_.serialization_budget_deferred_snapshot_count;
                continue;
            }
            const auto started = ReplicationClock::now();
            auto slices = world::make_chunk_snapshot_slices(*chunk);
            if (!slices) {
                return core::Status::failure(slices.error().code, slices.error().message);
            }
            EncodedChunkSnapshot encoded;
            encoded.identity = chunk->identity();
            encoded.content_revision = chunk->content_revision();
            encoded.slices.reserve(slices.value().size());
            for (const auto& slice : slices.value()) {
                encoded.slices.push_back(world::ChunkSnapshotSliceBinaryCodec::encode(slice));
            }
            current_chunk_subscriptions_.snapshot_serialization_time_us +=
                elapsed_replication_microseconds(started);
            ++current_chunk_subscriptions_.snapshot_serialization_operation_count;
            cached = snapshot_cache.insert_or_assign(coordinate, std::move(encoded)).first;
        }

        std::vector<net::TransportMessage> messages;
        messages.reserve(cached->second.slices.size());
        for (const auto& payload : cached->second.slices) {
            auto sequence = reserve_custom_replication_sequence();
            if (!sequence) {
                return core::Status::failure(sequence.error().code, sequence.error().message);
            }
            messages.push_back(world::make_encoded_chunk_snapshot_slice_message(
                payload, sequence.value(), current_time_ms_));
        }
        auto status = host_.send_reliable_replication_batch(client_id, std::move(messages));
        if (!status) {
            if (!net::is_host_session_reliable_backlog_capacity_error(status.error().code)) {
                return status;
            }
            const auto deferred = snapshot_candidates.size() - candidate_index;
            connection.deferred_chunk_snapshots += deferred;
            current_chunk_subscriptions_.deferred_snapshot_count += deferred;
            ++current_chunk_subscriptions_.reliable_admission_deferral_count;
            break;
        }
        connection.chunk_publications.insert_or_assign(
            coordinate,
            ChunkPublication{cached->second.identity, cached->second.content_revision, true});
        ++current_chunk_subscriptions_.snapshot_chunk_count;
        current_chunk_subscriptions_.snapshot_slice_message_count +=
            static_cast<std::uint32_t>(cached->second.slices.size());
        for (const auto& payload : cached->second.slices) {
            current_chunk_subscriptions_.snapshot_payload_bytes += payload.size();
        }
    }
    connection.chunk_subscriptions_converged =
        connection.deferred_chunk_additions == 0 && connection.deferred_chunk_removals == 0;
    return core::Status::ok();
}

bool ServerRuntime::initial_chunk_state_ready(const PlayerConnection& connection) const noexcept {
    if (desc_.direct_local_chunk_replication && renderer_proof_streaming_enabled_) {
        return true;
    }
    const auto publication_is_current = [this, &connection](world::ChunkCoord coordinate) {
        const auto* chunk = world_.chunks().find(coordinate);
        if (chunk == nullptr) {
            return false;
        }
        const auto publication = connection.chunk_publications.find(coordinate);
        return publication != connection.chunk_publications.end() && publication->second.complete &&
               publication->second.identity == chunk->identity() &&
               publication->second.content_revision == chunk->content_revision();
    };
    if (!publication_is_current(connection.chunk_subscription_center)) {
        return false;
    }
    return std::ranges::all_of(connection.chunk_subscriptions, [this, &publication_is_current](
                                                                   world::ChunkCoord coordinate) {
        return world_.chunks().find(coordinate) == nullptr || publication_is_current(coordinate);
    });
}

core::Status ServerRuntime::send_initial_state(core::NetId client_id) {
    const auto found = player_connections_.find(client_id.value());
    if (found == player_connections_.end()) {
        return core::Status::failure("server_runtime.player_not_connected",
                                     "initial state recipient has no active player");
    }
    auto& connection = found->second;
    if (connection.initial_state_published || !initial_chunk_state_ready(connection)) {
        return core::Status::ok();
    }

    std::vector<net::TransportMessage> messages;
    messages.reserve(world_.entities().records().size() + players_.records().size() + 2U);
    for (const auto* record : world_.entities().records()) {
        if (record->kind == entities::EntityKind::player || !record->net_id.is_valid()) {
            continue;
        }
        entities::EntityMotionSnapshot snapshot;
        snapshot.entity_net_id = record->net_id;
        snapshot.prototype_id = record->prototype_id;
        snapshot.previous_transform = record->transform;
        snapshot.current_transform = record->transform;
        snapshot.simulation_tick = 0;
        auto status = snapshot.validate();
        if (!status) {
            return status;
        }
        auto sequence = reserve_custom_replication_sequence();
        if (!sequence) {
            return core::Status::failure(sequence.error().code, sequence.error().message);
        }
        auto message = entities::make_entity_motion_snapshot_message(snapshot, sequence.value(),
                                                                     current_time_ms_);
        message.channel = net::TransportChannel::reliable;
        messages.push_back(std::move(message));
    }
    const auto* local_player = player_for_client(client_id);
    if (local_player == nullptr) {
        return core::Status::failure("server_runtime.player_record_missing",
                                     "connected client has no player assignment");
    }
    auto assignment_sequence = reserve_custom_replication_sequence();
    if (!assignment_sequence) {
        return core::Status::failure(assignment_sequence.error().code,
                                     assignment_sequence.error().message);
    }
    messages.push_back(movement::make_player_assignment_message(
        local_player->net_id, assignment_sequence.value(), current_time_ms_));
    for (const auto* player : players_.records()) {
        movement::PlayerControllerSnapshot snapshot;
        snapshot.player_net_id = player->net_id;
        snapshot.player_save_id = player->save_id;
        snapshot.state = player->state;
        snapshot.last_processed_input_sequence = player->state.last_input_sequence;
        snapshot.collision_world_revision = collision_world_revision();
        auto sequence = reserve_custom_replication_sequence();
        if (!sequence) {
            return core::Status::failure(sequence.error().code, sequence.error().message);
        }
        auto initial_snapshot =
            movement::make_movement_snapshot_message(snapshot, current_time_ms_, sequence.value());
        // The continuously replicated snapshot stream is latest-wins/unreliable. The bootstrap
        // snapshot is different: losing it leaves the client without a prediction seed, so carry
        // this one instance on the reliable session FIFO.
        initial_snapshot.channel = net::TransportChannel::reliable;
        messages.push_back(std::move(initial_snapshot));
    }
    auto inventory_sequence = reserve_custom_replication_sequence();
    if (!inventory_sequence) {
        return core::Status::failure(inventory_sequence.error().code,
                                     inventory_sequence.error().message);
    }
    net::ReplicationBatch inventory_batch;
    inventory_batch.command_sequence = inventory_sequence.value();
    inventory_batch.replication_sequence = inventory_sequence.value();
    inventory_batch.command_type = "world.initial_snapshot";
    inventory_batch.events.push_back(
        {"inventory.snapshot", local_player->save_id, "initial_private_inventory"});
    auto inventory_snapshot = world::materialize_replication_delta(world_, inventory_batch);
    auto inventory_message =
        world::make_replication_delta_transport_message(inventory_snapshot, current_time_ms_);
    if (!inventory_message) {
        return core::Status::failure(inventory_message.error().code,
                                     inventory_message.error().message);
    }
    messages.push_back(std::move(inventory_message).value());
    auto status = host_.send_reliable_replication_batch(client_id, std::move(messages));
    if (!status) {
        if (net::is_host_session_reliable_backlog_capacity_error(status.error().code)) {
            ++current_chunk_subscriptions_.reliable_admission_deferral_count;
            return core::Status::ok();
        }
        return status;
    }
    connection.initial_state_published = true;
    ++current_chunk_subscriptions_.published_initial_state_count;
    return core::Status::ok();
}

core::Result<std::uint64_t> ServerRuntime::reserve_custom_replication_sequence() {
    if (next_custom_replication_sequence_ == 0) {
        return core::Result<std::uint64_t>::failure(
            "server_runtime.replication_sequence_exhausted",
            "custom replication message sequence space is exhausted");
    }
    const auto sequence = next_custom_replication_sequence_;
    next_custom_replication_sequence_ =
        sequence == std::numeric_limits<std::uint64_t>::max() ? 0 : sequence + 1;
    return core::Result<std::uint64_t>::success(sequence);
}

std::uint64_t ServerRuntime::collision_world_revision() const noexcept {
    return collision_world_revision_;
}

} // namespace heartstead::game
