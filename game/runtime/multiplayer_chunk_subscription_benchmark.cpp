#include "game/runtime/multiplayer_chunk_subscription_benchmark.hpp"

#include "engine/content/content_validation.hpp"
#include "engine/net/transport_packet.hpp"
#include "engine/profiling/profiler.hpp"
#include "engine/world/chunks/chunk_replication.hpp"
#include "game/foundation/foundation_world.hpp"
#include "game/runtime/client_runtime.hpp"
#include "game/runtime/game_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <ranges>
#include <sstream>
#include <thread>
#include <utility>

namespace heartstead::game::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr double fixed_delta_seconds = 1.0 / 60.0;
constexpr std::int64_t maximum_path_component = 1'000'000;
constexpr std::uint32_t stable_warmup_ticks = 3;
constexpr std::size_t burst_chunks_per_client = 2;

struct BenchmarkClient {
    core::NetId id;
    ClientRuntime* runtime = nullptr;
    std::unique_ptr<ClientRuntime> owned_runtime;
};

[[nodiscard]] bool finite_positive(double value) noexcept {
    return std::isfinite(value) && value > 0.0;
}

[[nodiscard]] std::uint64_t elapsed_microseconds(BenchmarkClock::time_point begin,
                                                 BenchmarkClock::time_point end) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

[[nodiscard]] double percentile_ms(std::vector<std::uint64_t> values_us, std::uint32_t percentile) {
    if (values_us.empty()) {
        return 0.0;
    }
    std::ranges::sort(values_us);
    const auto rank = (values_us.size() * percentile + 99U) / 100U;
    const auto index = std::max<std::size_t>(1, rank) - 1;
    return static_cast<double>(values_us[index]) / 1'000.0;
}

[[nodiscard]] bool is_snapshot_message(const net::TransportEnvelope& envelope) noexcept {
    return envelope.message.payload_type == world::chunk_snapshot_slice_payload_type ||
           envelope.message.payload_type == world::legacy_chunk_snapshot_slice_payload_type;
}

[[nodiscard]] bool is_removal_message(const net::TransportEnvelope& envelope) noexcept {
    return envelope.message.payload_type == world::chunk_subscription_removal_payload_type;
}

[[nodiscard]] world::ChunkCoord offset_chunk(world::ChunkCoord coordinate, std::int64_t x,
                                             std::int64_t z) noexcept {
    coordinate.x += x;
    coordinate.z += z;
    return coordinate;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const auto character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned int>(static_cast<unsigned char>(character))
                       << std::dec << std::setfill(' ');
            } else {
                output << character;
            }
        }
    }
    output << '"';
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           std::string_view text) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.create_directory_failed",
                error.message());
        }
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.open_output_failed",
            "failed to open multiplayer chunk subscription benchmark output");
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.write_output_failed",
            "failed to write multiplayer chunk subscription benchmark output");
    }
    return core::Status::ok();
}

class BenchmarkRunner final {
  public:
    BenchmarkRunner(const MultiplayerChunkSubscriptionBenchmarkConfig& config,
                    const content::ContentValidationReport& content)
        : config_(config), content_(content) {}

    [[nodiscard]] core::Result<MultiplayerChunkSubscriptionBenchmarkReport> run() {
        HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.multiplayer_chunk_subscriptions");
        auto status = initialize();
        if (!status) {
            return failure(status);
        }

        status = run_transition(
            MultiplayerChunkSubscriptionPhase::cluster_transition, 0,
            std::vector<world::ChunkCoord>(clients_.size(), cluster_center_),
            std::vector<std::vector<world::ChunkCoord>>(clients_.size(), cluster_markers_));
        if (!status) {
            return failure(status);
        }

        status = run_transition(MultiplayerChunkSubscriptionPhase::spread_transition, 0,
                                spread_centers_, spread_markers_);
        if (!status) {
            return failure(status);
        }
        verify_cross_region_exclusions();

        for (std::uint32_t step = 0; step < config_.traversal_steps; ++step) {
            std::vector<std::vector<world::ChunkCoord>> expected_markers;
            expected_markers.reserve(clients_.size());
            for (std::size_t client = 0; client < clients_.size(); ++client) {
                expected_markers.push_back({traversal_centers_[step][client]});
            }
            status = run_transition(MultiplayerChunkSubscriptionPhase::traversal_transition, step,
                                    traversal_centers_[step], expected_markers);
            if (!status) {
                return failure(status);
            }
        }

        for (std::uint32_t tick = 0; tick < config_.steady_ticks; ++tick) {
            auto result = run_tick(MultiplayerChunkSubscriptionPhase::steady_state, tick, true);
            if (!result) {
                return failure(result.error().code, result.error().message);
            }
        }

        summarize();
        evaluate_gates();
        auto report_status = report_.validate();
        if (!report_status) {
            return failure(report_status);
        }
        return core::Result<MultiplayerChunkSubscriptionBenchmarkReport>::success(
            std::move(report_));
    }

  private:
    [[nodiscard]] core::Result<MultiplayerChunkSubscriptionBenchmarkReport>
    failure(const core::Status& status) const {
        return failure(status.error().code, status.error().message);
    }

    [[nodiscard]] core::Result<MultiplayerChunkSubscriptionBenchmarkReport>
    failure(std::string code, std::string message) const {
        return core::Result<MultiplayerChunkSubscriptionBenchmarkReport>::failure(
            std::move(code), std::move(message));
    }

    [[nodiscard]] core::Status initialize() {
        if (content_.has_errors()) {
            return core::Status::failure("multiplayer_chunk_subscription_benchmark.invalid_content",
                                         "benchmark content validation contains errors");
        }
        report_.config = config_;
        report_.runtime = profiling::query_runtime_metadata();

        auto initialized = GameRuntime::initialize({}, content_);
        if (!initialized) {
            return core::Status::failure(initialized.error().code, initialized.error().message);
        }
        runtime_ = std::move(initialized).value();

        RuntimeConfiguration runtime_config;
        runtime_config.fixed_step = {60, 4, 250'000};
        runtime_config.chunk_subscriptions = config_.subscriptions;
        runtime_config.max_chunk_snapshot_serialization_time_us_per_tick =
            config_.maximum_snapshot_serialization_time_us_per_tick;
        runtime_config.max_pending_reliable_messages_per_client = 256;
        runtime_config.max_pending_reliable_messages = config_.client_count * 256U;
        runtime_config.max_reliable_delivery_messages_per_client_per_tick =
            config_.reliable_delivery_messages_per_client_per_tick;
        runtime_config.max_reliable_delivery_messages_per_tick =
            config_.client_count * config_.reliable_delivery_messages_per_client_per_tick;

        auto metadata = content::save_metadata_from_content_report(
            content_, "multiplayer-chunk-subscription-benchmark", config_.seed);
        if (!metadata) {
            return core::Status::failure(metadata.error().code, metadata.error().message);
        }
        SessionRequest request;
        request.metadata = std::move(metadata).value();
        request.scenario_id = "base:scenarios/homestead";
        auto status = runtime_.start_session(runtime_config, std::move(request));
        if (!status) {
            return status;
        }
        session_ = runtime_.session();
        if (session_ == nullptr || session_->server() == nullptr || session_->client() == nullptr) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.missing_runtime",
                "automated session did not create a server and client runtime");
        }
        server_ = session_->server();
        clients_.push_back(
            BenchmarkClient{session_->client()->client_id(), session_->client(), nullptr});

        build_path();
        status = preload_marker_chunks();
        if (!status) {
            return status;
        }
        status = settle_unmeasured();
        if (!status) {
            return status;
        }

        for (std::uint32_t index = 1; index < config_.client_count; ++index) {
            auto connected = server_->connect_client();
            if (!connected) {
                return core::Status::failure(connected.error().code, connected.error().message);
            }
            world::WorldStateDesc world_desc;
            world_desc.metadata = server_->world().metadata();
            world_desc.voxel_palette = server_->world().voxel_palette_manifest();
            auto client = std::make_unique<ClientRuntime>(connected.value(), std::move(world_desc),
                                                          &server_->replication_registry(),
                                                          &server_->voxel_palette());
            auto* client_runtime = client.get();
            clients_.push_back(
                BenchmarkClient{connected.value(), client_runtime, std::move(client)});
            status = drain_and_synchronize(clients_.back(), nullptr, next_tick_);
            if (!status) {
                return status;
            }
        }
        std::ranges::sort(clients_, {}, &BenchmarkClient::id);
        status = settle_unmeasured();
        if (!status) {
            return status;
        }
        return core::Status::ok();
    }

    void build_path() {
        const auto seed_x = static_cast<std::int64_t>(config_.seed & 31U);
        const auto seed_z = static_cast<std::int64_t>((config_.seed >> 8U) & 31U);
        cluster_center_ = {256 + seed_x, 0, -256 - seed_z};
        cluster_markers_ = {cluster_center_, offset_chunk(cluster_center_, -1, 0)};

        spread_centers_.reserve(config_.client_count);
        spread_markers_.reserve(config_.client_count);
        for (std::uint32_t client = 0; client < config_.client_count; ++client) {
            const auto center = world::ChunkCoord{
                cluster_center_.x +
                    config_.spread_distance_chunks * (static_cast<std::int64_t>(client) + 1),
                0,
                cluster_center_.z + config_.spread_distance_chunks * 2,
            };
            spread_centers_.push_back(center);
            spread_markers_.push_back({center, offset_chunk(center, -1, 0)});
        }

        traversal_centers_.reserve(config_.traversal_steps);
        for (std::uint32_t step = 0; step < config_.traversal_steps; ++step) {
            std::vector<world::ChunkCoord> centers;
            centers.reserve(config_.client_count);
            for (const auto center : spread_centers_) {
                centers.push_back(offset_chunk(center, 0,
                                               config_.traversal_stride_chunks *
                                                   (static_cast<std::int64_t>(step) + 1)));
            }
            traversal_centers_.push_back(std::move(centers));
        }
    }

    [[nodiscard]] core::Status preload_marker_chunks() {
        const auto* foundation = server_->world().chunks().find({0, 0, 0});
        if (foundation == nullptr) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.missing_foundation_chunk",
                "foundation scenario did not create its origin chunk");
        }
        auto marker_cell = foundation->get({8, 0, 8});
        if (!marker_cell || marker_cell.value().is_air()) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.missing_marker_voxel",
                "foundation scenario did not provide a solid marker voxel");
        }

        std::vector<world::ChunkCoord> markers = cluster_markers_;
        for (const auto& client_markers : spread_markers_) {
            markers.insert(markers.end(), client_markers.begin(), client_markers.end());
        }
        for (const auto& step : traversal_centers_) {
            markers.insert(markers.end(), step.begin(), step.end());
        }
        std::ranges::sort(markers);
        markers.erase(std::unique(markers.begin(), markers.end()), markers.end());
        for (std::size_t index = 0; index < markers.size(); ++index) {
            auto& chunk = server_->world().chunks().get_or_create(markers[index]);
            const auto local = world::VoxelCoord{
                static_cast<std::uint16_t>(index % world::VoxelChunk::edge_length),
                static_cast<std::uint16_t>((index / world::VoxelChunk::edge_length) %
                                           world::VoxelChunk::edge_length),
                static_cast<std::uint16_t>(
                    (index / (world::VoxelChunk::edge_length * world::VoxelChunk::edge_length)) %
                    world::VoxelChunk::edge_length),
            };
            auto status = chunk.set(local, marker_cell.value());
            if (!status) {
                return status;
            }
            // Marker chunks exist to make relevance and serialization observable. Their lighting,
            // collision, and save work is settled before timing begins.
            chunk.clear_all_dirty();
        }
        return core::Status::ok();
    }

    [[nodiscard]] core::Status settle_unmeasured() {
        std::uint32_t stable_ticks = 0;
        for (std::uint32_t attempt = 0; attempt < config_.warmup_timeout_ticks; ++attempt) {
            auto tick_result = run_tick(MultiplayerChunkSubscriptionPhase::steady_state, 0, false);
            if (!tick_result) {
                return core::Status::failure(tick_result.error().code, tick_result.error().message);
            }
            const auto& stats = tick_result.value();
            const auto lighting_settled =
                stats.chunk_lighting.pending_relight_response_count == 0 &&
                stats.chunk_lighting.submitted_fields == stats.chunk_lighting.applied_fields &&
                stats.chunk_lighting.completed_mailbox_count == 0 &&
                stats.chunk_lighting.snapshot_cells_copied_this_update == 0;
            const auto collision_settled =
                stats.chunk_collision.pending_chunk_count == 0 &&
                stats.chunk_collision.in_flight_job_count == 0 &&
                stats.chunk_collision.completed_mailbox_count == 0 &&
                stats.chunk_collision.pending_collision_response_count == 0;
            const auto subscriptions_settled = subscription_state_is_current();
            if (lighting_settled && collision_settled && subscriptions_settled &&
                server_->host().pending_outbound_message_count() == 0) {
                ++stable_ticks;
                if (stable_ticks >= stable_warmup_ticks) {
                    return core::Status::ok();
                }
            } else {
                stable_ticks = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.warmup_timeout",
            "lighting, collision, subscription, or reliable work did not settle before timing");
    }

    [[nodiscard]] bool subscription_state_is_current() const {
        const auto snapshots = server_->chunk_subscription_clients();
        return snapshots.size() == clients_.size() &&
               std::ranges::all_of(snapshots, [](const auto& client) {
                   return client.converged && client.initial_state_published &&
                          client.partial_snapshot_count == 0 &&
                          client.stale_publication_count == 0 &&
                          client.deferred_snapshot_count == 0;
               });
    }

    [[nodiscard]] core::Status set_client_center(const BenchmarkClient& client,
                                                 world::ChunkCoord center) {
        auto block = world::chunk_local_to_block(center, {16, 1, 16});
        if (!block) {
            return core::Status::failure(block.error().code, block.error().message);
        }
        auto position = world::WorldPosition::from_anchor(block.value(), {0.5, 0.0, 0.5});
        if (!position) {
            return core::Status::failure(position.error().code, position.error().message);
        }
        auto* player = server_->player_for_client(client.id);
        if (player == nullptr) {
            return core::Status::failure("multiplayer_chunk_subscription_benchmark.missing_player",
                                         "benchmark client has no authoritative player");
        }
        player->state.position = position.value();
        player->state.fall_origin = position.value();
        player->state.scripted_start = position.value();
        player->state.scripted_target = position.value();
        return core::Status::ok();
    }

    [[nodiscard]] core::Status
    run_transition(MultiplayerChunkSubscriptionPhase phase, std::uint32_t ordinal,
                   const std::vector<world::ChunkCoord>& centers,
                   const std::vector<std::vector<world::ChunkCoord>>& expected_markers) {
        if (centers.size() != clients_.size() || expected_markers.size() != clients_.size()) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.invalid_transition",
                "transition center and marker counts must match the client count");
        }
        for (std::size_t index = 0; index < clients_.size(); ++index) {
            auto status = set_client_center(clients_[index], centers[index]);
            if (!status) {
                return status;
            }
        }

        MultiplayerChunkSubscriptionTransitionSample transition;
        transition.phase = phase;
        transition.ordinal = ordinal;
        for (std::uint32_t elapsed_ticks = 1; elapsed_ticks <= config_.transition_timeout_ticks;
             ++elapsed_ticks) {
            auto tick_result = run_tick(phase, ordinal, true);
            if (!tick_result) {
                return core::Status::failure(tick_result.error().code, tick_result.error().message);
            }
            transition.peak_pending_reliable_messages =
                std::max(transition.peak_pending_reliable_messages,
                         server_->host().pending_outbound_message_count());
            if (transition_is_complete(centers, expected_markers)) {
                transition.ticks_to_converge = elapsed_ticks;
                transition.converged = true;
                report_.transitions.push_back(transition);
                return core::Status::ok();
            }
        }
        return core::Status::failure("multiplayer_chunk_subscription_benchmark.transition_timeout",
                                     std::string(multiplayer_chunk_subscription_phase_name(phase)) +
                                         " did not converge before the configured tick bound");
    }

    [[nodiscard]] bool transition_is_complete(
        const std::vector<world::ChunkCoord>& centers,
        const std::vector<std::vector<world::ChunkCoord>>& expected_markers) const {
        if (server_->host().pending_outbound_message_count() != 0) {
            return false;
        }
        auto snapshots = server_->chunk_subscription_clients();
        if (snapshots.size() != clients_.size()) {
            return false;
        }
        std::ranges::sort(snapshots, {}, &ServerChunkSubscriptionClientSnapshot::client_id);
        for (std::size_t index = 0; index < clients_.size(); ++index) {
            const auto& snapshot = snapshots[index];
            if (snapshot.client_id != clients_[index].id || snapshot.center != centers[index] ||
                !snapshot.converged || !snapshot.initial_state_published ||
                snapshot.partial_snapshot_count != 0 || snapshot.stale_publication_count != 0 ||
                snapshot.deferred_snapshot_count != 0 || clients_[index].runtime == nullptr ||
                !clients_[index].runtime->is_connected()) {
                return false;
            }
            for (const auto coordinate : expected_markers[index]) {
                if (!clients_[index].runtime->world().chunks().contains(coordinate)) {
                    return false;
                }
            }
        }
        return true;
    }

    [[nodiscard]] core::Result<ServerRuntimeTickStats>
    run_tick(MultiplayerChunkSubscriptionPhase phase, std::uint32_t phase_ordinal, bool record) {
        now_ms_ += 17;
        const auto started = BenchmarkClock::now();
        auto result = server_->run_tick(next_tick_, fixed_delta_seconds, now_ms_);
        const auto elapsed_us = elapsed_microseconds(started, BenchmarkClock::now());
        if (!result) {
            return result;
        }

        MultiplayerChunkSubscriptionTickSample sample;
        sample.phase = phase;
        sample.phase_ordinal = phase_ordinal;
        sample.tick = next_tick_++;
        sample.server_tick_time_us = elapsed_us;
        const auto& subscription = result.value().chunk_subscriptions;
        sample.connected_client_count =
            static_cast<std::uint32_t>(server_->host().connected_client_count());
        sample.subscription_count = subscription.subscription_count;
        sample.maximum_client_subscription_count = subscription.maximum_client_subscription_count;
        sample.converged_client_count = subscription.converged_client_count;
        sample.added_subscription_count = subscription.added_subscription_count;
        sample.removed_subscription_count = subscription.removed_subscription_count;
        sample.removal_message_count = subscription.removal_message_count;
        sample.partial_snapshot_count = subscription.partial_snapshot_count;
        sample.stale_publication_count = subscription.stale_publication_count;
        sample.deferred_addition_count = subscription.deferred_addition_count;
        sample.deferred_removal_count = subscription.deferred_removal_count;
        sample.deferred_snapshot_count = subscription.deferred_snapshot_count;
        sample.snapshot_chunk_count = subscription.snapshot_chunk_count;
        sample.snapshot_slice_message_count = subscription.snapshot_slice_message_count;
        sample.snapshot_serialization_operation_count =
            subscription.snapshot_serialization_operation_count;
        sample.snapshot_payload_bytes = subscription.snapshot_payload_bytes;
        sample.snapshot_serialization_time_us = subscription.snapshot_serialization_time_us;
        sample.snapshot_serialization_time_overshoot_us =
            subscription.snapshot_serialization_time_overshoot_us;
        sample.serialization_budget_deferred_snapshot_count =
            subscription.serialization_budget_deferred_snapshot_count;
        sample.reliable_admission_deferral_count = subscription.reliable_admission_deferral_count;
        sample.pending_reliable_message_count = server_->host().pending_outbound_message_count();
        sample.pending_reliable_bytes = server_->host().pending_outbound_bytes();
        sample.disconnected_client_count = static_cast<std::uint32_t>(
            result.value().commands.disconnected_clients.size() +
            result.value().commands.outbound_delivery.overload_disconnected_clients.size());

        sample.clients.reserve(clients_.size());
        for (auto& client : clients_) {
            MultiplayerChunkClientTickTraffic traffic;
            traffic.client_id = client.id;
            auto status = drain_and_synchronize(client, &traffic, sample.tick);
            if (!status) {
                return core::Result<ServerRuntimeTickStats>::failure(status.error().code,
                                                                     status.error().message);
            }
            sample.clients.push_back(traffic);
        }
        if (record) {
            report_.raw_ticks.push_back(std::move(sample));
        }
        return result;
    }

    [[nodiscard]] core::Status drain_and_synchronize(BenchmarkClient& client,
                                                     MultiplayerChunkClientTickTraffic* traffic,
                                                     std::uint64_t render_tick) {
        auto messages = server_->drain_client_messages(client.id);
        if (!messages) {
            return core::Status::failure(messages.error().code, messages.error().message);
        }
        if (traffic != nullptr) {
            for (const auto& envelope : messages.value()) {
                const auto wire_bytes = net::TransportPacketCodec::encode(envelope).size();
                if (envelope.message.channel == net::TransportChannel::reliable) {
                    ++traffic->reliable_messages;
                    traffic->reliable_wire_bytes += wire_bytes;
                } else {
                    ++traffic->unreliable_messages;
                    traffic->unreliable_wire_bytes += wire_bytes;
                }
                if (is_snapshot_message(envelope)) {
                    ++traffic->chunk_snapshot_slice_messages;
                    traffic->chunk_snapshot_wire_bytes += wire_bytes;
                } else if (is_removal_message(envelope)) {
                    ++traffic->chunk_removal_messages;
                    traffic->chunk_removal_wire_bytes += wire_bytes;
                }
            }
        }
        auto status = client.runtime->receive(messages.value());
        if (!status) {
            return status;
        }
        auto synchronized =
            client.runtime->synchronize(render_tick, std::numeric_limits<std::size_t>::max());
        if (!synchronized) {
            return core::Status::failure(synchronized.error().code, synchronized.error().message);
        }
        if (traffic != nullptr) {
            traffic->completed_chunk_snapshots =
                synchronized.value().completed_chunk_snapshot_count;
            traffic->applied_chunk_removals = synchronized.value().chunk_subscription_removal_count;
        }
        if (!client.runtime->is_connected()) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.client_disconnected",
                "a benchmark client disconnected during the measured workload");
        }
        return core::Status::ok();
    }

    void verify_cross_region_exclusions() {
        report_.summary.expected_cross_region_exclusions =
            static_cast<std::uint64_t>(clients_.size()) *
            static_cast<std::uint64_t>(clients_.size() - 1U) * burst_chunks_per_client;
        auto snapshots = server_->chunk_subscription_clients();
        std::ranges::sort(snapshots, {}, &ServerChunkSubscriptionClientSnapshot::client_id);
        for (std::size_t client = 0; client < clients_.size(); ++client) {
            for (std::size_t region = 0; region < clients_.size(); ++region) {
                if (client == region) {
                    continue;
                }
                for (const auto marker : spread_markers_[region]) {
                    const auto absent_on_client =
                        !clients_[client].runtime->world().chunks().contains(marker);
                    const auto absent_on_server =
                        !std::ranges::binary_search(snapshots[client].subscriptions, marker);
                    if (absent_on_client && absent_on_server) {
                        ++report_.summary.verified_cross_region_exclusions;
                    }
                }
            }
        }
    }

    void summarize() {
        auto& summary = report_.summary;
        summary.measured_tick_count = report_.raw_ticks.size();
        std::vector<std::uint64_t> tick_times;
        tick_times.reserve(report_.raw_ticks.size());
        std::map<core::NetId, MultiplayerChunkClientTrafficSummary> client_totals;
        bool backlog_active = false;
        std::uint32_t backlog_recovery_ticks = 0;

        for (const auto& sample : report_.raw_ticks) {
            tick_times.push_back(sample.server_tick_time_us);
            summary.maximum_client_subscription_count =
                std::max(summary.maximum_client_subscription_count,
                         sample.maximum_client_subscription_count);
            summary.maximum_added_subscriptions_per_tick = std::max(
                summary.maximum_added_subscriptions_per_tick, sample.added_subscription_count);
            summary.maximum_removed_subscriptions_per_tick = std::max(
                summary.maximum_removed_subscriptions_per_tick, sample.removed_subscription_count);
            summary.maximum_partial_snapshot_count =
                std::max(summary.maximum_partial_snapshot_count, sample.partial_snapshot_count);
            summary.maximum_stale_publication_count =
                std::max(summary.maximum_stale_publication_count, sample.stale_publication_count);
            summary.maximum_deferred_snapshot_count =
                std::max(summary.maximum_deferred_snapshot_count, sample.deferred_snapshot_count);
            summary.maximum_snapshot_serialization_time_us =
                std::max(summary.maximum_snapshot_serialization_time_us,
                         sample.snapshot_serialization_time_us);
            summary.maximum_snapshot_serialization_time_overshoot_us =
                std::max(summary.maximum_snapshot_serialization_time_overshoot_us,
                         sample.snapshot_serialization_time_overshoot_us);
            summary.serialization_budget_deferred_snapshot_count +=
                sample.serialization_budget_deferred_snapshot_count;
            summary.peak_pending_reliable_messages = std::max(
                summary.peak_pending_reliable_messages, sample.pending_reliable_message_count);
            summary.peak_pending_reliable_bytes =
                std::max(summary.peak_pending_reliable_bytes, sample.pending_reliable_bytes);
            summary.disconnected_client_count += sample.disconnected_client_count;

            if (sample.phase == MultiplayerChunkSubscriptionPhase::cluster_transition) {
                summary.cluster_snapshot_chunk_count += sample.snapshot_chunk_count;
                summary.cluster_snapshot_serialization_operation_count +=
                    sample.snapshot_serialization_operation_count;
            } else if (sample.phase == MultiplayerChunkSubscriptionPhase::spread_transition) {
                summary.spread_snapshot_chunk_count += sample.snapshot_chunk_count;
                summary.spread_snapshot_serialization_operation_count +=
                    sample.snapshot_serialization_operation_count;
            }

            if (!backlog_active && sample.pending_reliable_message_count != 0) {
                backlog_active = true;
                backlog_recovery_ticks = 0;
                ++summary.observed_backlog_burst_count;
            } else if (backlog_active) {
                ++backlog_recovery_ticks;
                if (sample.pending_reliable_message_count == 0) {
                    summary.maximum_backlog_recovery_ticks =
                        std::max(summary.maximum_backlog_recovery_ticks, backlog_recovery_ticks);
                    backlog_active = false;
                }
            }

            for (const auto& traffic : sample.clients) {
                auto& total = client_totals[traffic.client_id];
                total.client_id = traffic.client_id;
                total.reliable_messages += traffic.reliable_messages;
                total.unreliable_messages += traffic.unreliable_messages;
                total.chunk_snapshot_slice_messages += traffic.chunk_snapshot_slice_messages;
                total.chunk_removal_messages += traffic.chunk_removal_messages;
                total.reliable_wire_bytes += traffic.reliable_wire_bytes;
                total.unreliable_wire_bytes += traffic.unreliable_wire_bytes;
                total.chunk_snapshot_wire_bytes += traffic.chunk_snapshot_wire_bytes;
                total.chunk_removal_wire_bytes += traffic.chunk_removal_wire_bytes;
                const auto wire_bytes = traffic.reliable_wire_bytes + traffic.unreliable_wire_bytes;
                total.maximum_wire_bytes_per_tick =
                    std::max(total.maximum_wire_bytes_per_tick, wire_bytes);
                summary.reliable_wire_bytes += traffic.reliable_wire_bytes;
                summary.unreliable_wire_bytes += traffic.unreliable_wire_bytes;
                summary.maximum_wire_bytes_per_client_per_tick =
                    std::max(summary.maximum_wire_bytes_per_client_per_tick, wire_bytes);
            }
        }

        summary.server_tick_p50_ms = percentile_ms(tick_times, 50);
        summary.server_tick_p95_ms = percentile_ms(tick_times, 95);
        summary.server_tick_p99_ms = percentile_ms(tick_times, 99);
        summary.maximum_server_tick_ms =
            tick_times.empty()
                ? 0.0
                : static_cast<double>(*std::ranges::max_element(tick_times)) / 1'000.0;
        for (const auto& transition : report_.transitions) {
            summary.maximum_transition_convergence_ticks = std::max(
                summary.maximum_transition_convergence_ticks, transition.ticks_to_converge);
        }
        if (summary.cluster_snapshot_serialization_operation_count != 0) {
            summary.shared_snapshot_reuse_ratio =
                static_cast<double>(summary.cluster_snapshot_chunk_count) /
                static_cast<double>(summary.cluster_snapshot_serialization_operation_count);
        }
        if (summary.spread_snapshot_serialization_operation_count != 0) {
            summary.disjoint_snapshot_reuse_ratio =
                static_cast<double>(summary.spread_snapshot_chunk_count) /
                static_cast<double>(summary.spread_snapshot_serialization_operation_count);
        }
        for (auto& [_, total] : client_totals) {
            summary.clients.push_back(std::move(total));
        }
        summary.final_pending_reliable_messages = server_->host().pending_outbound_message_count();
        summary.final_pending_reliable_bytes = server_->host().pending_outbound_bytes();
        const auto final_clients = server_->chunk_subscription_clients();
        summary.final_converged_client_count =
            static_cast<std::uint32_t>(std::ranges::count_if(final_clients, [](const auto& client) {
                return client.converged && client.initial_state_published &&
                       client.partial_snapshot_count == 0 && client.stale_publication_count == 0 &&
                       client.deferred_snapshot_count == 0;
            }));
    }

    void evaluate_gates() {
        auto& gates = report_.gates;
        const auto& summary = report_.summary;
        gates.evaluated = true;
        const auto maximum = [&gates](std::string metric, double actual, double limit) {
            if (actual > limit) {
                gates.violations.push_back({std::move(metric), actual, limit});
            }
        };
        const auto minimum = [&gates](std::string metric, double actual, double limit) {
            if (actual < limit) {
                gates.violations.push_back({std::move(metric), actual, limit});
            }
        };
        maximum("server_tick_p95_ms", summary.server_tick_p95_ms,
                config_.maximum_server_tick_p95_ms);
        maximum("server_tick_p99_ms", summary.server_tick_p99_ms,
                config_.maximum_server_tick_p99_ms);
        maximum("maximum_server_tick_ms", summary.maximum_server_tick_ms,
                config_.maximum_server_tick_ms);
        maximum("maximum_transition_convergence_ticks",
                summary.maximum_transition_convergence_ticks,
                config_.maximum_transition_convergence_ticks);
        maximum("maximum_backlog_recovery_ticks", summary.maximum_backlog_recovery_ticks,
                config_.maximum_backlog_recovery_ticks);
        minimum("observed_backlog_burst_count", summary.observed_backlog_burst_count, 1.0);
        minimum("shared_snapshot_reuse_ratio", summary.shared_snapshot_reuse_ratio,
                config_.minimum_shared_snapshot_reuse_ratio);
        maximum("disjoint_snapshot_reuse_ratio", summary.disjoint_snapshot_reuse_ratio,
                config_.maximum_disjoint_snapshot_reuse_ratio);
        maximum("maximum_snapshot_serialization_time_overshoot_us",
                static_cast<double>(summary.maximum_snapshot_serialization_time_overshoot_us),
                static_cast<double>(config_.maximum_snapshot_serialization_time_overshoot_us));
        maximum("maximum_wire_bytes_per_client_per_tick",
                static_cast<double>(summary.maximum_wire_bytes_per_client_per_tick),
                static_cast<double>(config_.maximum_wire_bytes_per_client_per_tick));
        maximum("maximum_client_subscription_count",
                static_cast<double>(summary.maximum_client_subscription_count),
                static_cast<double>(config_.subscriptions.max_chunks_per_client));
        maximum("maximum_added_subscriptions_per_tick",
                summary.maximum_added_subscriptions_per_tick,
                static_cast<double>(config_.subscriptions.max_additions_per_update) *
                    config_.client_count);
        maximum("maximum_removed_subscriptions_per_tick",
                summary.maximum_removed_subscriptions_per_tick,
                static_cast<double>(config_.subscriptions.max_removals_per_update) *
                    config_.client_count);
        maximum("maximum_partial_snapshot_count",
                static_cast<double>(summary.maximum_partial_snapshot_count), 0.0);
        maximum("maximum_stale_publication_count",
                static_cast<double>(summary.maximum_stale_publication_count), 0.0);
        maximum("final_pending_reliable_messages",
                static_cast<double>(summary.final_pending_reliable_messages), 0.0);
        maximum("disconnected_client_count", summary.disconnected_client_count, 0.0);
        minimum("verified_cross_region_exclusions",
                static_cast<double>(summary.verified_cross_region_exclusions),
                static_cast<double>(summary.expected_cross_region_exclusions));
        minimum("final_converged_client_count", summary.final_converged_client_count,
                config_.client_count);
        gates.passed = gates.violations.empty();
    }

    const MultiplayerChunkSubscriptionBenchmarkConfig& config_;
    const content::ContentValidationReport& content_;
    MultiplayerChunkSubscriptionBenchmarkReport report_;
    GameRuntime runtime_;
    RuntimeSession* session_ = nullptr;
    ServerRuntime* server_ = nullptr;
    std::vector<BenchmarkClient> clients_;
    world::ChunkCoord cluster_center_;
    std::vector<world::ChunkCoord> cluster_markers_;
    std::vector<world::ChunkCoord> spread_centers_;
    std::vector<std::vector<world::ChunkCoord>> spread_markers_;
    std::vector<std::vector<world::ChunkCoord>> traversal_centers_;
    std::uint64_t next_tick_ = 1;
    std::int64_t now_ms_ = 0;
};

void write_runtime_metadata(std::ostream& output, const profiling::RuntimeMetadata& runtime) {
    output << "{\n      \"engine_version\": ";
    write_json_string(output, runtime.engine_version);
    output << ",\n      \"git_commit\": ";
    write_json_string(output, runtime.git_commit);
    output << ",\n      \"git_dirty\": " << (runtime.git_dirty ? "true" : "false")
           << ",\n      \"build_configuration\": ";
    write_json_string(output, runtime.build_configuration);
    output << ",\n      \"compiler\": ";
    write_json_string(output, runtime.compiler);
    output << ",\n      \"platform\": ";
    write_json_string(output, runtime.platform);
    output << ",\n      \"architecture\": ";
    write_json_string(output, runtime.architecture);
    output << ",\n      \"operating_system\": ";
    write_json_string(output, runtime.operating_system);
    output << ",\n      \"cpu_model\": ";
    write_json_string(output, runtime.cpu_model);
    output << ",\n      \"logical_cpu_count\": " << runtime.logical_cpu_count
           << ",\n      \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false")
           << "\n    }";
}

void write_client_traffic(std::ostream& output, const MultiplayerChunkClientTickTraffic& traffic) {
    output << "{\"client_id\": " << traffic.client_id.value()
           << ", \"reliable_messages\": " << traffic.reliable_messages
           << ", \"unreliable_messages\": " << traffic.unreliable_messages
           << ", \"chunk_snapshot_slice_messages\": " << traffic.chunk_snapshot_slice_messages
           << ", \"chunk_removal_messages\": " << traffic.chunk_removal_messages
           << ", \"reliable_wire_bytes\": " << traffic.reliable_wire_bytes
           << ", \"unreliable_wire_bytes\": " << traffic.unreliable_wire_bytes
           << ", \"chunk_snapshot_wire_bytes\": " << traffic.chunk_snapshot_wire_bytes
           << ", \"chunk_removal_wire_bytes\": " << traffic.chunk_removal_wire_bytes
           << ", \"completed_chunk_snapshots\": " << traffic.completed_chunk_snapshots
           << ", \"applied_chunk_removals\": " << traffic.applied_chunk_removals << '}';
}

} // namespace

std::string_view
multiplayer_chunk_subscription_phase_name(MultiplayerChunkSubscriptionPhase phase) noexcept {
    switch (phase) {
    case MultiplayerChunkSubscriptionPhase::cluster_transition:
        return "cluster_transition";
    case MultiplayerChunkSubscriptionPhase::spread_transition:
        return "spread_transition";
    case MultiplayerChunkSubscriptionPhase::traversal_transition:
        return "traversal_transition";
    case MultiplayerChunkSubscriptionPhase::steady_state:
        return "steady_state";
    }
    return "unknown";
}

core::Status MultiplayerChunkSubscriptionBenchmarkConfig::validate() const {
    auto status = subscriptions.validate();
    if (!status) {
        return status;
    }
    if (client_count < 2 || client_count > 32 || traversal_steps == 0 || steady_ticks == 0 ||
        warmup_timeout_ticks == 0 || transition_timeout_ticks == 0) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_workload",
            "benchmark requires 2-32 clients and nonzero traversal, steady, warmup, and transition "
            "bounds");
    }
    const auto minimum_spread =
        static_cast<std::int64_t>(subscriptions.retain_horizontal_radius_chunks) * 2 + 1;
    if (spread_distance_chunks < minimum_spread ||
        spread_distance_chunks > maximum_path_component ||
        traversal_stride_chunks <=
            static_cast<std::int64_t>(subscriptions.subscribe_horizontal_radius_chunks) ||
        traversal_stride_chunks > maximum_path_component ||
        traversal_steps > static_cast<std::uint32_t>(maximum_path_component)) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_path",
            "spread must keep client volumes disjoint and traversal must leave the prior subscribe "
            "radius within the deterministic path bound");
    }
    const auto messages_per_snapshot = static_cast<std::uint32_t>(world::VoxelChunk::edge_length);
    if (reliable_delivery_messages_per_client_per_tick < messages_per_snapshot ||
        reliable_delivery_messages_per_client_per_tick >=
            messages_per_snapshot * burst_chunks_per_client) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_delivery_budget",
            "delivery budget must admit one snapshot and defer part of the two-snapshot stress "
            "burst");
    }
    if (!finite_positive(maximum_server_tick_p95_ms) ||
        !finite_positive(maximum_server_tick_p99_ms) || !finite_positive(maximum_server_tick_ms) ||
        maximum_transition_convergence_ticks == 0 || maximum_backlog_recovery_ticks == 0 ||
        !finite_positive(minimum_shared_snapshot_reuse_ratio) ||
        !finite_positive(maximum_disjoint_snapshot_reuse_ratio) ||
        maximum_snapshot_serialization_time_us_per_tick == 0 ||
        maximum_wire_bytes_per_client_per_tick == 0) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_gates",
            "latency, convergence, reuse, serialization, and wire-byte gates must be positive");
    }
    return core::Status::ok();
}

core::Status MultiplayerChunkSubscriptionBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto expected_transitions = static_cast<std::size_t>(config.traversal_steps) + 2U;
    if (raw_ticks.empty() || raw_ticks.size() != summary.measured_tick_count ||
        transitions.size() != expected_transitions ||
        summary.clients.size() != config.client_count) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.incomplete_report",
            "benchmark report is missing tick, transition, or per-client evidence");
    }
    std::uint64_t previous_tick = 0;
    for (const auto& sample : raw_ticks) {
        if (sample.tick <= previous_tick || sample.connected_client_count != config.client_count ||
            sample.clients.size() != config.client_count || sample.disconnected_client_count != 0 ||
            sample.partial_snapshot_count != 0 ||
            sample.maximum_client_subscription_count > config.subscriptions.max_chunks_per_client ||
            sample.added_subscription_count >
                config.subscriptions.max_additions_per_update * config.client_count ||
            sample.removed_subscription_count >
                config.subscriptions.max_removals_per_update * config.client_count ||
            sample.snapshot_slice_message_count !=
                sample.snapshot_chunk_count * world::VoxelChunk::edge_length) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.invalid_tick_sample",
                "raw tick evidence violates connection, subscription, or atomic snapshot bounds");
        }
        previous_tick = sample.tick;
        if (!std::ranges::is_sorted(sample.clients, {},
                                    &MultiplayerChunkClientTickTraffic::client_id) ||
            std::ranges::adjacent_find(sample.clients, {},
                                       &MultiplayerChunkClientTickTraffic::client_id) !=
                sample.clients.end()) {
            return core::Status::failure(
                "multiplayer_chunk_subscription_benchmark.invalid_client_order",
                "per-tick client traffic must be unique and deterministically ordered");
        }
    }
    if (std::ranges::any_of(transitions, [](const auto& transition) {
            return !transition.converged || transition.ticks_to_converge == 0;
        })) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.incomplete_transition",
            "every cluster, spread, and traversal transition must converge");
    }
    if (summary.observed_backlog_burst_count == 0 ||
        summary.cluster_snapshot_serialization_operation_count == 0 ||
        summary.spread_snapshot_serialization_operation_count == 0 ||
        summary.final_pending_reliable_messages != 0 || summary.final_pending_reliable_bytes != 0 ||
        summary.final_converged_client_count != config.client_count ||
        summary.disconnected_client_count != 0 || summary.maximum_partial_snapshot_count != 0 ||
        summary.maximum_stale_publication_count != 0 ||
        summary.verified_cross_region_exclusions != summary.expected_cross_region_exclusions) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_final_state",
            "benchmark did not retain sharing, disjoint relevance, backlog recovery, and clean "
            "final-state evidence");
    }
    if (!std::isfinite(summary.server_tick_p50_ms) || !std::isfinite(summary.server_tick_p95_ms) ||
        !std::isfinite(summary.server_tick_p99_ms) ||
        !std::isfinite(summary.maximum_server_tick_ms) ||
        !std::isfinite(summary.shared_snapshot_reuse_ratio) ||
        !std::isfinite(summary.disjoint_snapshot_reuse_ratio)) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_metrics",
            "benchmark summary contains non-finite timing or reuse metrics");
    }
    if (!gates.evaluated || gates.passed != gates.violations.empty()) {
        return core::Status::failure(
            "multiplayer_chunk_subscription_benchmark.invalid_gate_evaluation",
            "gate pass state must be evaluated and agree with retained violations");
    }
    return core::Status::ok();
}

bool MultiplayerChunkSubscriptionBenchmarkReport::gates_passed() const noexcept {
    return gates.evaluated && gates.passed;
}

std::string MultiplayerChunkSubscriptionBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n  \"schema_version\": " << schema_version
           << ",\n  \"benchmark\": \"multiplayer_chunk_subscriptions\",\n  \"runtime\": ";
    write_runtime_metadata(output, runtime);
    output << ",\n  \"config\": {\n"
           << "    \"seed\": " << config.seed << ",\n"
           << "    \"client_count\": " << config.client_count << ",\n"
           << "    \"traversal_steps\": " << config.traversal_steps << ",\n"
           << "    \"steady_ticks\": " << config.steady_ticks << ",\n"
           << "    \"warmup_timeout_ticks\": " << config.warmup_timeout_ticks << ",\n"
           << "    \"transition_timeout_ticks\": " << config.transition_timeout_ticks << ",\n"
           << "    \"spread_distance_chunks\": " << config.spread_distance_chunks << ",\n"
           << "    \"traversal_stride_chunks\": " << config.traversal_stride_chunks << ",\n"
           << "    \"reliable_delivery_messages_per_client_per_tick\": "
           << config.reliable_delivery_messages_per_client_per_tick << ",\n"
           << "    \"subscribe_horizontal_radius_chunks\": "
           << config.subscriptions.subscribe_horizontal_radius_chunks << ",\n"
           << "    \"subscribe_vertical_radius_chunks\": "
           << config.subscriptions.subscribe_vertical_radius_chunks << ",\n"
           << "    \"retain_horizontal_radius_chunks\": "
           << config.subscriptions.retain_horizontal_radius_chunks << ",\n"
           << "    \"retain_vertical_radius_chunks\": "
           << config.subscriptions.retain_vertical_radius_chunks << ",\n"
           << "    \"max_chunks_per_client\": " << config.subscriptions.max_chunks_per_client
           << ",\n"
           << "    \"max_additions_per_update\": " << config.subscriptions.max_additions_per_update
           << ",\n"
           << "    \"max_removals_per_update\": " << config.subscriptions.max_removals_per_update
           << ",\n"
           << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false")
           << ",\n    \"maximum_server_tick_p95_ms\": " << config.maximum_server_tick_p95_ms
           << ",\n    \"maximum_server_tick_p99_ms\": " << config.maximum_server_tick_p99_ms
           << ",\n    \"maximum_server_tick_ms\": " << config.maximum_server_tick_ms
           << ",\n    \"maximum_transition_convergence_ticks\": "
           << config.maximum_transition_convergence_ticks
           << ",\n    \"maximum_backlog_recovery_ticks\": " << config.maximum_backlog_recovery_ticks
           << ",\n    \"minimum_shared_snapshot_reuse_ratio\": "
           << config.minimum_shared_snapshot_reuse_ratio
           << ",\n    \"maximum_disjoint_snapshot_reuse_ratio\": "
           << config.maximum_disjoint_snapshot_reuse_ratio
           << ",\n    \"maximum_snapshot_serialization_time_us_per_tick\": "
           << config.maximum_snapshot_serialization_time_us_per_tick
           << ",\n    \"maximum_snapshot_serialization_time_overshoot_us\": "
           << config.maximum_snapshot_serialization_time_overshoot_us
           << ",\n    \"maximum_wire_bytes_per_client_per_tick\": "
           << config.maximum_wire_bytes_per_client_per_tick << "\n  },\n";

    const auto& value = summary;
    output
        << "  \"summary\": {\n"
        << "    \"measured_tick_count\": " << value.measured_tick_count << ",\n"
        << "    \"server_tick_p50_ms\": " << value.server_tick_p50_ms << ",\n"
        << "    \"server_tick_p95_ms\": " << value.server_tick_p95_ms << ",\n"
        << "    \"server_tick_p99_ms\": " << value.server_tick_p99_ms << ",\n"
        << "    \"maximum_server_tick_ms\": " << value.maximum_server_tick_ms << ",\n"
        << "    \"maximum_transition_convergence_ticks\": "
        << value.maximum_transition_convergence_ticks << ",\n"
        << "    \"observed_backlog_burst_count\": " << value.observed_backlog_burst_count << ",\n"
        << "    \"maximum_backlog_recovery_ticks\": " << value.maximum_backlog_recovery_ticks
        << ",\n"
        << "    \"peak_pending_reliable_messages\": " << value.peak_pending_reliable_messages
        << ",\n"
        << "    \"peak_pending_reliable_bytes\": " << value.peak_pending_reliable_bytes
        << ",\n    \"maximum_client_subscription_count\": "
        << value.maximum_client_subscription_count
        << ",\n    \"maximum_added_subscriptions_per_tick\": "
        << value.maximum_added_subscriptions_per_tick
        << ",\n    \"maximum_removed_subscriptions_per_tick\": "
        << value.maximum_removed_subscriptions_per_tick
        << ",\n    \"maximum_partial_snapshot_count\": " << value.maximum_partial_snapshot_count
        << ",\n    \"maximum_stale_publication_count\": " << value.maximum_stale_publication_count
        << ",\n    \"maximum_deferred_snapshot_count\": " << value.maximum_deferred_snapshot_count
        << ",\n    \"maximum_snapshot_serialization_time_us\": "
        << value.maximum_snapshot_serialization_time_us
        << ",\n    \"maximum_snapshot_serialization_time_overshoot_us\": "
        << value.maximum_snapshot_serialization_time_overshoot_us
        << ",\n    \"serialization_budget_deferred_snapshot_count\": "
        << value.serialization_budget_deferred_snapshot_count
        << ",\n    \"cluster_snapshot_chunk_count\": " << value.cluster_snapshot_chunk_count
        << ",\n    \"cluster_snapshot_serialization_operation_count\": "
        << value.cluster_snapshot_serialization_operation_count
        << ",\n    \"shared_snapshot_reuse_ratio\": " << value.shared_snapshot_reuse_ratio
        << ",\n    \"spread_snapshot_chunk_count\": " << value.spread_snapshot_chunk_count
        << ",\n    \"spread_snapshot_serialization_operation_count\": "
        << value.spread_snapshot_serialization_operation_count
        << ",\n    \"disjoint_snapshot_reuse_ratio\": " << value.disjoint_snapshot_reuse_ratio
        << ",\n    \"reliable_wire_bytes\": " << value.reliable_wire_bytes
        << ",\n    \"unreliable_wire_bytes\": " << value.unreliable_wire_bytes
        << ",\n    \"maximum_wire_bytes_per_client_per_tick\": "
        << value.maximum_wire_bytes_per_client_per_tick
        << ",\n    \"verified_cross_region_exclusions\": " << value.verified_cross_region_exclusions
        << ",\n    \"expected_cross_region_exclusions\": " << value.expected_cross_region_exclusions
        << ",\n    \"final_pending_reliable_messages\": " << value.final_pending_reliable_messages
        << ",\n    \"final_pending_reliable_bytes\": " << value.final_pending_reliable_bytes
        << ",\n    \"final_converged_client_count\": " << value.final_converged_client_count
        << ",\n    \"disconnected_client_count\": " << value.disconnected_client_count
        << ",\n    \"clients\": [\n";
    for (std::size_t index = 0; index < value.clients.size(); ++index) {
        const auto& client = value.clients[index];
        output << "      {\"client_id\": " << client.client_id.value()
               << ", \"reliable_messages\": " << client.reliable_messages
               << ", \"unreliable_messages\": " << client.unreliable_messages
               << ", \"chunk_snapshot_slice_messages\": " << client.chunk_snapshot_slice_messages
               << ", \"chunk_removal_messages\": " << client.chunk_removal_messages
               << ", \"reliable_wire_bytes\": " << client.reliable_wire_bytes
               << ", \"unreliable_wire_bytes\": " << client.unreliable_wire_bytes
               << ", \"chunk_snapshot_wire_bytes\": " << client.chunk_snapshot_wire_bytes
               << ", \"chunk_removal_wire_bytes\": " << client.chunk_removal_wire_bytes
               << ", \"maximum_wire_bytes_per_tick\": " << client.maximum_wire_bytes_per_tick << '}'
               << (index + 1 == value.clients.size() ? "\n" : ",\n");
    }
    output << "    ]\n  },\n  \"transitions\": [\n";
    for (std::size_t index = 0; index < transitions.size(); ++index) {
        const auto& transition = transitions[index];
        output << "    {\"phase\": ";
        write_json_string(output, multiplayer_chunk_subscription_phase_name(transition.phase));
        output << ", \"ordinal\": " << transition.ordinal
               << ", \"ticks_to_converge\": " << transition.ticks_to_converge
               << ", \"peak_pending_reliable_messages\": "
               << transition.peak_pending_reliable_messages
               << ", \"converged\": " << (transition.converged ? "true" : "false") << '}'
               << (index + 1 == transitions.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"raw_ticks\": [\n";
    for (std::size_t index = 0; index < raw_ticks.size(); ++index) {
        const auto& sample = raw_ticks[index];
        output << "    {\"phase\": ";
        write_json_string(output, multiplayer_chunk_subscription_phase_name(sample.phase));
        output << ", \"phase_ordinal\": " << sample.phase_ordinal << ", \"tick\": " << sample.tick
               << ", \"server_tick_time_us\": " << sample.server_tick_time_us
               << ", \"connected_client_count\": " << sample.connected_client_count
               << ", \"subscription_count\": " << sample.subscription_count
               << ", \"maximum_client_subscription_count\": "
               << sample.maximum_client_subscription_count
               << ", \"converged_client_count\": " << sample.converged_client_count
               << ", \"added_subscription_count\": " << sample.added_subscription_count
               << ", \"removed_subscription_count\": " << sample.removed_subscription_count
               << ", \"removal_message_count\": " << sample.removal_message_count
               << ", \"partial_snapshot_count\": " << sample.partial_snapshot_count
               << ", \"stale_publication_count\": " << sample.stale_publication_count
               << ", \"deferred_addition_count\": " << sample.deferred_addition_count
               << ", \"deferred_removal_count\": " << sample.deferred_removal_count
               << ", \"deferred_snapshot_count\": " << sample.deferred_snapshot_count
               << ", \"snapshot_chunk_count\": " << sample.snapshot_chunk_count
               << ", \"snapshot_slice_message_count\": " << sample.snapshot_slice_message_count
               << ", \"snapshot_serialization_operation_count\": "
               << sample.snapshot_serialization_operation_count
               << ", \"snapshot_payload_bytes\": " << sample.snapshot_payload_bytes
               << ", \"snapshot_serialization_time_us\": " << sample.snapshot_serialization_time_us
               << ", \"snapshot_serialization_time_overshoot_us\": "
               << sample.snapshot_serialization_time_overshoot_us
               << ", \"serialization_budget_deferred_snapshot_count\": "
               << sample.serialization_budget_deferred_snapshot_count
               << ", \"reliable_admission_deferral_count\": "
               << sample.reliable_admission_deferral_count
               << ", \"pending_reliable_message_count\": " << sample.pending_reliable_message_count
               << ", \"pending_reliable_bytes\": " << sample.pending_reliable_bytes
               << ", \"disconnected_client_count\": " << sample.disconnected_client_count
               << ", \"clients\": [";
        for (std::size_t client = 0; client < sample.clients.size(); ++client) {
            if (client != 0) {
                output << ", ";
            }
            write_client_traffic(output, sample.clients[client]);
        }
        output << "]}" << (index + 1 == raw_ticks.size() ? "\n" : ",\n");
    }
    output << "  ],\n  \"gates\": {\n    \"evaluated\": " << (gates.evaluated ? "true" : "false")
           << ",\n    \"passed\": " << (gates.passed ? "true" : "false")
           << ",\n    \"violations\": [";
    if (!gates.violations.empty()) {
        output << '\n';
    }
    for (std::size_t index = 0; index < gates.violations.size(); ++index) {
        const auto& violation = gates.violations[index];
        output << "      {\"metric\": ";
        write_json_string(output, violation.metric);
        output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit << '}'
               << (index + 1 == gates.violations.size() ? "\n" : ",\n");
    }
    output << "    ]\n  }\n}\n";
    return output.str();
}

core::Status
MultiplayerChunkSubscriptionBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    if (!status) {
        return status;
    }
    return write_text_file(path, to_json());
}

core::Result<MultiplayerChunkSubscriptionBenchmarkReport>
run_multiplayer_chunk_subscription_benchmark(
    const MultiplayerChunkSubscriptionBenchmarkConfig& config,
    const content::ContentValidationReport& content) {
    auto status = config.validate();
    if (!status) {
        return core::Result<MultiplayerChunkSubscriptionBenchmarkReport>::failure(
            status.error().code, status.error().message);
    }
    BenchmarkRunner runner(config, content);
    return runner.run();
}

} // namespace heartstead::game::benchmark
