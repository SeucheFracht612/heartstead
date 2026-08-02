#include "engine/world/benchmark/voxel_response_benchmark.hpp"

#include "engine/physics/physics_world.hpp"
#include "engine/profiling/profiler.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace heartstead::world::benchmark {

namespace {

using BenchmarkClock = dirty::DirtyRegionClock;

constexpr ChunkCoord benchmark_center{};
constexpr std::uint16_t edit_margin = 4;

struct BenchmarkState {
    VoxelPalette palette;
    ChunkDatabase chunks;
    dirty::DirtyRegionTracker dirty_regions;
    std::unique_ptr<physics::IPhysicsWorld> physics_world;
    std::unique_ptr<physics::ChunkCollisionSystem> collision;
    std::unique_ptr<ChunkLightSystem> lighting;
    std::vector<ChunkCoord> coordinates;
};

[[nodiscard]] std::size_t configured_chunk_count(std::uint16_t radius) noexcept {
    const auto side = static_cast<std::size_t>(radius) * 2U + 1U;
    return side * side;
}

[[nodiscard]] std::vector<ChunkCoord> benchmark_coordinates(std::uint16_t radius) {
    std::vector<ChunkCoord> result;
    result.reserve(configured_chunk_count(radius));
    const auto signed_radius = static_cast<std::int64_t>(radius);
    for (std::int64_t z = -signed_radius; z <= signed_radius; ++z) {
        for (std::int64_t x = -signed_radius; x <= signed_radius; ++x) {
            result.push_back({benchmark_center.x + x, benchmark_center.y, benchmark_center.z + z});
        }
    }
    std::ranges::sort(result, [](ChunkCoord lhs, ChunkCoord rhs) {
        const auto lhs_distance = lhs.x * lhs.x + lhs.z * lhs.z;
        const auto rhs_distance = rhs.x * rhs.x + rhs.z * rhs.z;
        return lhs_distance != rhs_distance ? lhs_distance < rhs_distance : lhs < rhs;
    });
    return result;
}

[[nodiscard]] bool is_benchmark_coordinate(ChunkCoord coord, std::uint16_t radius) noexcept {
    const auto signed_radius = static_cast<std::int64_t>(radius);
    return coord.y == benchmark_center.y && coord.x >= benchmark_center.x - signed_radius &&
           coord.x <= benchmark_center.x + signed_radius &&
           coord.z >= benchmark_center.z - signed_radius &&
           coord.z <= benchmark_center.z + signed_radius;
}

[[nodiscard]] VoxelCoord edit_voxel(std::uint64_t ordinal) noexcept {
    constexpr auto interior_width = VoxelChunk::edge_length - edit_margin * 2U;
    return {
        static_cast<std::uint16_t>(edit_margin + (ordinal * 7U) % interior_width),
        1,
        static_cast<std::uint16_t>(edit_margin + (ordinal * 11U) % interior_width),
    };
}

[[nodiscard]] std::uint64_t elapsed_microseconds(BenchmarkClock::time_point begin,
                                                 BenchmarkClock::time_point end) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

[[nodiscard]] core::Result<VoxelPalette> make_palette() {
    const auto prototype = core::PrototypeId::parse("benchmark:voxels/response_stone");
    if (!prototype) {
        return core::Result<VoxelPalette>::failure(
            "voxel_response_benchmark.invalid_voxel_id",
            "the built-in response benchmark voxel id is invalid");
    }
    VoxelDefinition solid;
    solid.type = 1;
    solid.prototype_id = *prototype;
    solid.display_name = "Response benchmark stone";
    solid.terrain_material = "benchmark_stone";
    solid.mining_tool = "pickaxe";
    solid.light_absorption = maximum_voxel_light;

    VoxelPalette palette;
    auto status = palette.add(std::move(solid));
    if (!status) {
        return core::Result<VoxelPalette>::failure(status.error().code, status.error().message);
    }
    return core::Result<VoxelPalette>::success(std::move(palette));
}

[[nodiscard]] std::vector<VoxelCell> flat_chunk_cells() {
    std::vector<VoxelCell> cells(VoxelChunk::total_cells, VoxelCell::air());
    constexpr auto edge = static_cast<std::size_t>(VoxelChunk::edge_length);
    for (std::size_t z = 0; z < edge; ++z) {
        for (std::size_t x = 0; x < edge; ++x) {
            cells[z * edge * edge + x] = VoxelCell{1, 0};
        }
    }
    return cells;
}

[[nodiscard]] core::Result<std::unique_ptr<BenchmarkState>>
make_state(const VoxelResponseBenchmarkConfig& config) {
    auto state = std::make_unique<BenchmarkState>();
    auto palette = make_palette();
    if (!palette) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(palette.error().code,
                                                                      palette.error().message);
    }
    state->palette = std::move(palette).value();
    state->coordinates = benchmark_coordinates(config.horizontal_radius_chunks);
    const auto cells = flat_chunk_cells();
    for (const auto coordinate : state->coordinates) {
        VoxelChunk chunk(coordinate);
        auto status = chunk.load_generated_cells(cells);
        if (!status) {
            return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                          status.error().message);
        }
        status = state->chunks.insert_generated(std::move(chunk), state->dirty_regions);
        if (!status) {
            return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                          status.error().message);
        }
    }

    auto physics_world = physics::create_physics_world({.backend = config.physics_backend});
    if (!physics_world) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(
            physics_world.error().code, physics_world.error().message);
    }
    state->physics_world = std::move(physics_world).value();
    auto collision = physics::ChunkCollisionSystem::create(*state->physics_world, state->palette,
                                                           config.collision);
    if (!collision) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(collision.error().code,
                                                                      collision.error().message);
    }
    state->collision = std::move(collision).value();
    auto lighting = ChunkLightSystem::create(state->palette, config.lighting);
    if (!lighting) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(lighting.error().code,
                                                                      lighting.error().message);
    }
    state->lighting = std::move(lighting).value();
    return core::Result<std::unique_ptr<BenchmarkState>>::success(std::move(state));
}

void discard_unowned_dirty_regions(dirty::DirtyRegionTracker& dirty_regions) {
    static_cast<void>(dirty_regions.consume_kind(dirty::DirtyRegionKind::chunk_mesh));
    static_cast<void>(dirty_regions.consume_kind(dirty::DirtyRegionKind::water_network));
}

[[nodiscard]] core::Status update_systems(BenchmarkState& state) {
    auto status = state.collision->update(state.chunks, state.dirty_regions, state.palette);
    if (!status) {
        return status;
    }
    status = state.lighting->update(state.chunks, state.dirty_regions, state.palette);
    discard_unowned_dirty_regions(state.dirty_regions);
    return status;
}

[[nodiscard]] std::pair<std::size_t, std::size_t>
current_stage_counts(const BenchmarkState& state) {
    std::size_t collision = 0;
    std::size_t lighting = 0;
    for (const auto identity : state.chunks.identities()) {
        const auto* chunk = state.chunks.find(identity.coordinate);
        if (chunk == nullptr || chunk->identity() != identity) {
            continue;
        }
        collision += chunk->stages().record(ChunkStage::collision).resident_is_current() ? 1U : 0U;
        lighting += chunk->stages().record(ChunkStage::lighting).resident_is_current() ? 1U : 0U;
    }
    return {collision, lighting};
}

[[nodiscard]] bool systems_are_idle(BenchmarkState& state) {
    const auto collision = state.collision->stats();
    const auto lighting = state.lighting->stats();
    const auto [current_collision, current_lighting] = current_stage_counts(state);
    return collision.pending_chunk_count == 0 && collision.in_flight_job_count == 0 &&
           collision.completed_mailbox_count == 0 &&
           collision.pending_collision_response_count == 0 && !lighting.relight_requested &&
           !lighting.snapshot_in_progress && !lighting.solve_in_flight &&
           lighting.completed_mailbox_count == 0 && lighting.pending_relight_response_count == 0 &&
           current_collision == state.coordinates.size() &&
           current_lighting == state.coordinates.size();
}

void pace_owner_thread(BenchmarkClock::time_point& next_update, BenchmarkClock::time_point deadline,
                       std::uint64_t interval_us) {
    next_update += std::chrono::microseconds(interval_us);
    std::this_thread::sleep_until(std::min(next_update, deadline));
}

[[nodiscard]] core::Status settle_initial_state(BenchmarkState& state,
                                                const VoxelResponseBenchmarkConfig& config) {
    const auto start = BenchmarkClock::now();
    const auto deadline = start + std::chrono::milliseconds(config.timeout_ms);
    auto next_update = start;
    while (true) {
        auto status = update_systems(state);
        if (!status) {
            return status;
        }
        if (systems_are_idle(state)) {
            return core::Status::ok();
        }
        if (BenchmarkClock::now() >= deadline) {
            return core::Status::failure(
                "voxel_response_benchmark.initial_timeout",
                "collision and lighting systems did not settle for the initial chunk corpus");
        }
        pace_owner_thread(next_update, deadline, config.update_interval_us);
    }
}

[[nodiscard]] core::Result<VoxelResponseBenchmarkSample>
execute_edit(BenchmarkState& state, const VoxelResponseBenchmarkConfig& config,
             std::uint64_t operation_ordinal, std::uint32_t repetition) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.voxel_response.edit");
    const auto edit_slot = operation_ordinal / 2U;
    const auto coordinate =
        state.coordinates[static_cast<std::size_t>(edit_slot % state.coordinates.size())];
    const auto voxel = edit_voxel(edit_slot);
    const auto previous = state.chunks.get(coordinate, voxel);
    if (!previous) {
        return core::Result<VoxelResponseBenchmarkSample>::failure(previous.error().code,
                                                                   previous.error().message);
    }
    const auto next = previous.value().type == 1 ? VoxelCell::air() : VoxelCell{1, 0};

    const auto collision_before = state.collision->stats();
    const auto lighting_before = state.lighting->stats();
    auto status = state.chunks.set(coordinate, voxel, next, state.dirty_regions, state.palette);
    if (!status) {
        return core::Result<VoxelResponseBenchmarkSample>::failure(status.error().code,
                                                                   status.error().message);
    }

    VoxelResponseBenchmarkSample sample;
    sample.repetition = repetition;
    sample.coord = coordinate;
    sample.voxel = voxel;
    sample.became_solid = next.type == 1;
    const auto start = BenchmarkClock::now();
    const auto deadline = start + std::chrono::milliseconds(config.timeout_ms);
    auto next_update = start;
    while (true) {
        const auto owner_update_start = BenchmarkClock::now();
        status = update_systems(state);
        sample.maximum_owner_update_ms = std::max(
            sample.maximum_owner_update_ms,
            std::chrono::duration<double, std::milli>(BenchmarkClock::now() - owner_update_start)
                .count());
        if (!status) {
            return core::Result<VoxelResponseBenchmarkSample>::failure(status.error().code,
                                                                       status.error().message);
        }
        ++sample.owner_updates;
        const auto collision = state.collision->stats();
        const auto lighting = state.lighting->stats();
        if (collision.applied_this_update > 0) {
            sample.maximum_collision_cooking_ms =
                std::max(sample.maximum_collision_cooking_ms, collision.last_cooking_ms);
        }
        sample.maximum_collision_apply_ms =
            std::max(sample.maximum_collision_apply_ms, collision.last_apply_ms);
        sample.maximum_relight_solve_ms =
            std::max(sample.maximum_relight_solve_ms, lighting.last_solve_ms);
        sample.maximum_relight_apply_ms =
            std::max(sample.maximum_relight_apply_ms, lighting.last_apply_ms);
        sample.snapshot_cells_copied += lighting.snapshot_cells_copied_this_update;
        sample.relight_changed_chunks += lighting.changed_chunks_this_update;
        sample.relight_changed_cells += lighting.changed_cells_this_update;

        const auto collision_completions = collision.total_collision_response_completed -
                                           collision_before.total_collision_response_completed;
        const auto relight_completions = lighting.total_relight_response_completed -
                                         lighting_before.total_relight_response_completed;
        const auto collision_coalesced = collision.total_coalesced_collision_invalidations -
                                         collision_before.total_coalesced_collision_invalidations;
        const auto relight_coalesced = lighting.total_coalesced_relight_invalidations -
                                       lighting_before.total_coalesced_relight_invalidations;
        const auto collision_abandoned = collision.total_abandoned_collision_invalidations -
                                         collision_before.total_abandoned_collision_invalidations;
        const auto relight_abandoned = lighting.total_abandoned_relight_invalidations -
                                       lighting_before.total_abandoned_relight_invalidations;
        const auto collision_failed = collision.failed_results - collision_before.failed_results;
        const auto relight_failed = lighting.failed_results - lighting_before.failed_results;
        if (collision_completions > 1 || relight_completions > 1 || collision_coalesced != 0 ||
            relight_coalesced != 0 || collision_abandoned != 0 || relight_abandoned != 0 ||
            collision_failed != 0 || relight_failed != 0) {
            return core::Result<VoxelResponseBenchmarkSample>::failure(
                "voxel_response_benchmark.invalid_edit_lifecycle",
                "an isolated edit was coalesced, abandoned, failed, or completed more than once");
        }

        const auto idle = systems_are_idle(state);
        if (collision_completions == 1 && relight_completions == 1 && idle) {
            if (collision.collision_response_latency.sample_count !=
                    collision_before.collision_response_latency.sample_count + 1U ||
                lighting.relight_convergence_latency.sample_count !=
                    lighting_before.relight_convergence_latency.sample_count + 1U) {
                return core::Result<VoxelResponseBenchmarkSample>::failure(
                    "voxel_response_benchmark.missing_latency_sample",
                    "an edit completed without exactly one collision and relight latency sample");
            }
            sample.collision_response_ms = collision.collision_response_latency.latest_ms;
            sample.relight_convergence_ms = lighting.relight_convergence_latency.latest_ms;
            sample.collision_stale_results =
                collision.stale_results - collision_before.stale_results;
            sample.relight_stale_snapshots =
                lighting.stale_snapshots - lighting_before.stale_snapshots;
            sample.relight_stale_results = lighting.stale_results - lighting_before.stale_results;
            sample.relight_apply_budget_overruns =
                lighting.apply_budget_overruns - lighting_before.apply_budget_overruns;
            return core::Result<VoxelResponseBenchmarkSample>::success(std::move(sample));
        }
        if (idle) {
            return core::Result<VoxelResponseBenchmarkSample>::failure(
                "voxel_response_benchmark.censored_edit",
                "collision or lighting became idle without publishing a response sample");
        }
        if (BenchmarkClock::now() >= deadline) {
            return core::Result<VoxelResponseBenchmarkSample>::failure(
                "voxel_response_benchmark.edit_timeout",
                "an isolated edit did not converge within the configured timeout");
        }
        pace_owner_thread(next_update, deadline, config.update_interval_us);
    }
}

[[nodiscard]] double percentile(const std::vector<double>& sorted, double fraction) noexcept {
    if (sorted.empty()) {
        return 0.0;
    }
    const auto position = fraction * static_cast<double>(sorted.size() - 1U);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const auto upper = static_cast<std::size_t>(std::ceil(position));
    const auto weight = position - static_cast<double>(lower);
    return sorted[lower] * (1.0 - weight) + sorted[upper] * weight;
}

[[nodiscard]] std::string json_escape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const auto character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

void write_json_string(std::ostream& output, std::string_view value) {
    output << '"' << json_escape(value) << '"';
}

void write_runtime_metadata(std::ostream& output, const profiling::RuntimeMetadata& runtime) {
    output << "  \"runtime\": {\n";
    const auto string_field = [&output](std::string_view name, std::string_view value,
                                        bool trailing_comma = true) {
        output << "    \"" << name << "\": ";
        write_json_string(output, value);
        output << (trailing_comma ? ",\n" : "\n");
    };
    string_field("engine_version", runtime.engine_version);
    string_field("git_commit", runtime.git_commit);
    string_field("build_configuration", runtime.build_configuration);
    string_field("compiler", runtime.compiler);
    string_field("platform", runtime.platform);
    string_field("architecture", runtime.architecture);
    string_field("operating_system", runtime.operating_system);
    string_field("cpu_model", runtime.cpu_model);
    output << "    \"logical_cpu_count\": " << runtime.logical_cpu_count << ",\n"
           << "    \"git_dirty\": " << (runtime.git_dirty ? "true" : "false") << ",\n"
           << "    \"tracy_enabled\": " << (runtime.tracy_enabled ? "true" : "false") << "\n  },\n";
}

[[nodiscard]] core::Status write_text_file(const std::filesystem::path& path,
                                           std::string_view text) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return core::Status::failure("voxel_response_benchmark.create_directory_failed",
                                         "failed to create benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("voxel_response_benchmark.open_output_failed",
                                     "failed to open benchmark output: " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        return core::Status::failure("voxel_response_benchmark.write_output_failed",
                                     "failed to write benchmark output: " + path.string());
    }
    return core::Status::ok();
}

} // namespace

core::Status VoxelResponseBenchmarkConfig::validate() const {
    const auto backend = physics::physics_backend_info(physics_backend);
    if (!backend.available) {
        return core::Status::failure(
            "voxel_response_benchmark.unavailable_physics_backend",
            "response benchmark physics backend is unknown or unavailable: " +
                std::string(backend.name));
    }
    if (horizontal_radius_chunks > 4) {
        return core::Status::failure(
            "voxel_response_benchmark.invalid_radius",
            "response benchmark radius must be between zero and four chunks");
    }
    if (repetitions == 0 || repetitions > 100 || warmup_repetitions > 100) {
        return core::Status::failure(
            "voxel_response_benchmark.invalid_repetitions",
            "response benchmark repetitions must be 1..100 and warmups 0..100");
    }
    if (update_interval_us == 0 || update_interval_us > 1'000'000 || timeout_ms == 0 ||
        timeout_ms > 600'000) {
        return core::Status::failure(
            "voxel_response_benchmark.invalid_timing",
            "owner update cadence and edit timeout must be positive and bounded");
    }
    if (!std::isfinite(maximum_collision_p95_ms) || maximum_collision_p95_ms <= 0.0 ||
        !std::isfinite(maximum_relight_p95_ms) || maximum_relight_p95_ms <= 0.0) {
        return core::Status::failure("voxel_response_benchmark.invalid_gates",
                                     "collision and relight P95 gates must be finite and positive");
    }
    auto status = collision.validate();
    return status ? lighting.validate() : status;
}

core::Status VoxelResponseBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto expected_chunks = configured_chunk_count(config.horizontal_radius_chunks);
    if (run.chunk_count != expected_chunks || run.warmup_edits != config.warmup_repetitions ||
        run.measured_edits != config.repetitions || raw_samples.size() != config.repetitions) {
        return core::Status::failure(
            "voxel_response_benchmark.incomplete_report",
            "response report does not contain its configured corpus and edit samples");
    }
    if (run.collision_response_completions != config.repetitions ||
        run.relight_response_completions != config.repetitions ||
        run.collision_coalesced_invalidations != 0 || run.relight_coalesced_invalidations != 0 ||
        run.collision_abandoned_invalidations != 0 || run.relight_abandoned_invalidations != 0 ||
        run.collision_failed_results != 0 || run.relight_failed_results != 0 ||
        run.pending_collision_responses != 0 || run.pending_relight_responses != 0 ||
        run.current_collision_stages != expected_chunks ||
        run.current_lighting_stages != expected_chunks) {
        return core::Status::failure(
            "voxel_response_benchmark.incomplete_lifecycle",
            "response run contains failed, coalesced, abandoned, pending, or non-current work");
    }

    std::set<std::uint32_t> repetitions;
    std::uint64_t owner_updates = 0;
    for (const auto& sample : raw_samples) {
        const bool valid_voxel = sample.voxel.y == 1 && sample.voxel.x >= edit_margin &&
                                 sample.voxel.z >= edit_margin &&
                                 sample.voxel.x + edit_margin < VoxelChunk::edge_length &&
                                 sample.voxel.z + edit_margin < VoxelChunk::edge_length;
        const auto finite_nonnegative = [](double value) {
            return std::isfinite(value) && value >= 0.0;
        };
        if (sample.repetition >= config.repetitions ||
            !repetitions.insert(sample.repetition).second ||
            !is_benchmark_coordinate(sample.coord, config.horizontal_radius_chunks) ||
            !valid_voxel || sample.owner_updates == 0 ||
            !std::isfinite(sample.collision_response_ms) || sample.collision_response_ms <= 0.0 ||
            !std::isfinite(sample.relight_convergence_ms) || sample.relight_convergence_ms <= 0.0 ||
            !finite_nonnegative(sample.maximum_collision_cooking_ms) ||
            !finite_nonnegative(sample.maximum_collision_apply_ms) ||
            !finite_nonnegative(sample.maximum_relight_solve_ms) ||
            !finite_nonnegative(sample.maximum_relight_apply_ms) ||
            !finite_nonnegative(sample.maximum_owner_update_ms) ||
            sample.snapshot_cells_copied < expected_chunks * VoxelChunk::total_cells) {
            return core::Status::failure(
                "voxel_response_benchmark.invalid_sample",
                "response report contains a missing, censored, or invalid raw sample");
        }
        owner_updates += sample.owner_updates;
    }
    if (run.owner_updates != owner_updates || run.elapsed_us == 0) {
        return core::Status::failure("voxel_response_benchmark.invalid_run_totals",
                                     "response run totals do not match its retained raw samples");
    }
    return core::Status::ok();
}

VoxelResponseBenchmarkSummary VoxelResponseBenchmarkReport::summary() const {
    VoxelResponseBenchmarkSummary result;
    result.sample_count = raw_samples.size();
    std::vector<double> collision;
    std::vector<double> relight;
    collision.reserve(raw_samples.size());
    relight.reserve(raw_samples.size());
    double owner_updates = 0.0;
    for (const auto& sample : raw_samples) {
        collision.push_back(sample.collision_response_ms);
        relight.push_back(sample.relight_convergence_ms);
        owner_updates += static_cast<double>(sample.owner_updates);
        result.maximum_collision_cooking_ms =
            std::max(result.maximum_collision_cooking_ms, sample.maximum_collision_cooking_ms);
        result.maximum_collision_apply_ms =
            std::max(result.maximum_collision_apply_ms, sample.maximum_collision_apply_ms);
        result.maximum_relight_solve_ms =
            std::max(result.maximum_relight_solve_ms, sample.maximum_relight_solve_ms);
        result.maximum_relight_apply_ms =
            std::max(result.maximum_relight_apply_ms, sample.maximum_relight_apply_ms);
        result.maximum_owner_update_ms =
            std::max(result.maximum_owner_update_ms, sample.maximum_owner_update_ms);
        result.total_snapshot_cells_copied += sample.snapshot_cells_copied;
    }
    std::ranges::sort(collision);
    std::ranges::sort(relight);
    result.median_collision_response_ms = percentile(collision, 0.50);
    result.p95_collision_response_ms = percentile(collision, 0.95);
    result.p99_collision_response_ms = percentile(collision, 0.99);
    result.maximum_collision_response_ms = collision.empty() ? 0.0 : collision.back();
    result.median_relight_convergence_ms = percentile(relight, 0.50);
    result.p95_relight_convergence_ms = percentile(relight, 0.95);
    result.p99_relight_convergence_ms = percentile(relight, 0.99);
    result.maximum_relight_convergence_ms = relight.empty() ? 0.0 : relight.back();
    result.mean_owner_updates_per_edit =
        raw_samples.empty() ? 0.0 : owner_updates / static_cast<double>(raw_samples.size());

    result.gates.evaluated = config.enforce_gates;
    if (config.enforce_gates) {
        const auto check = [&result](std::string metric, double actual, double limit) {
            if (!std::isfinite(actual) || actual > limit) {
                result.gates.violations.push_back({std::move(metric), actual, limit});
            }
        };
        check("p95_collision_response_ms", result.p95_collision_response_ms,
              config.maximum_collision_p95_ms);
        check("p95_relight_convergence_ms", result.p95_relight_convergence_ms,
              config.maximum_relight_p95_ms);
        result.gates.passed = result.gates.violations.empty();
    }
    return result;
}

bool VoxelResponseBenchmarkReport::gates_passed() const {
    return summary().gates.passed;
}

std::string VoxelResponseBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n"
           << "  \"schema_version\": " << schema_version << ",\n"
           << "  \"benchmark\": \"voxel_response\",\n"
           << "  \"config\": {\n"
           << "    \"physics_backend\": \"" << physics::physics_backend_name(config.physics_backend)
           << "\",\n"
           << "    \"horizontal_radius_chunks\": " << config.horizontal_radius_chunks << ",\n"
           << "    \"warmup_repetitions\": " << config.warmup_repetitions << ",\n"
           << "    \"repetitions\": " << config.repetitions << ",\n"
           << "    \"update_interval_us\": " << config.update_interval_us << ",\n"
           << "    \"timeout_ms\": " << config.timeout_ms << ",\n"
           << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n"
           << "    \"maximum_collision_p95_ms\": " << config.maximum_collision_p95_ms << ",\n"
           << "    \"maximum_relight_p95_ms\": " << config.maximum_relight_p95_ms << ",\n"
           << "    \"collision\": {\"workers\": " << config.collision.scheduler.worker_count
           << ", \"max_concurrent_jobs\": " << config.collision.scheduler.max_concurrent_jobs
           << ", \"max_submissions_per_update\": " << config.collision.max_submissions_per_update
           << ", \"max_applies_per_update\": " << config.collision.max_applies_per_update
           << ", \"apply_time_budget_ms\": " << config.collision.apply_time_budget_ms << "},\n"
           << "    \"lighting\": {\"workers\": " << config.lighting.scheduler.worker_count
           << ", \"max_snapshot_cells_per_update\": "
           << config.lighting.max_snapshot_cells_per_update
           << ", \"apply_time_budget_ms\": " << config.lighting.apply_time_budget_ms << "}\n"
           << "  },\n";
    write_runtime_metadata(output, runtime);
    output << "  \"run\": {\n"
           << "    \"chunk_count\": " << run.chunk_count << ",\n"
           << "    \"warmup_edits\": " << run.warmup_edits << ",\n"
           << "    \"measured_edits\": " << run.measured_edits << ",\n"
           << "    \"owner_updates\": " << run.owner_updates << ",\n"
           << "    \"elapsed_us\": " << run.elapsed_us << ",\n"
           << "    \"collision_response_completions\": " << run.collision_response_completions
           << ",\n"
           << "    \"relight_response_completions\": " << run.relight_response_completions << ",\n"
           << "    \"collision_coalesced_invalidations\": " << run.collision_coalesced_invalidations
           << ",\n"
           << "    \"relight_coalesced_invalidations\": " << run.relight_coalesced_invalidations
           << ",\n"
           << "    \"collision_abandoned_invalidations\": " << run.collision_abandoned_invalidations
           << ",\n"
           << "    \"relight_abandoned_invalidations\": " << run.relight_abandoned_invalidations
           << ",\n"
           << "    \"collision_stale_results\": " << run.collision_stale_results << ",\n"
           << "    \"relight_stale_snapshots\": " << run.relight_stale_snapshots << ",\n"
           << "    \"relight_stale_results\": " << run.relight_stale_results << ",\n"
           << "    \"collision_failed_results\": " << run.collision_failed_results << ",\n"
           << "    \"relight_failed_results\": " << run.relight_failed_results << ",\n"
           << "    \"relight_apply_budget_overruns\": " << run.relight_apply_budget_overruns
           << ",\n"
           << "    \"pending_collision_responses\": " << run.pending_collision_responses << ",\n"
           << "    \"pending_relight_responses\": " << run.pending_relight_responses << ",\n"
           << "    \"current_collision_stages\": " << run.current_collision_stages << ",\n"
           << "    \"current_lighting_stages\": " << run.current_lighting_stages << "\n"
           << "  },\n";

    output << "  \"raw_samples\": [\n";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output << "    {\"repetition\": " << sample.repetition << ", \"coord\": [" << sample.coord.x
               << ", " << sample.coord.y << ", " << sample.coord.z << "], \"voxel\": ["
               << sample.voxel.x << ", " << sample.voxel.y << ", " << sample.voxel.z
               << "], \"became_solid\": " << (sample.became_solid ? "true" : "false")
               << ", \"owner_updates\": " << sample.owner_updates
               << ", \"collision_response_ms\": " << sample.collision_response_ms
               << ", \"relight_convergence_ms\": " << sample.relight_convergence_ms
               << ", \"maximum_collision_cooking_ms\": " << sample.maximum_collision_cooking_ms
               << ", \"maximum_collision_apply_ms\": " << sample.maximum_collision_apply_ms
               << ", \"maximum_relight_solve_ms\": " << sample.maximum_relight_solve_ms
               << ", \"maximum_relight_apply_ms\": " << sample.maximum_relight_apply_ms
               << ", \"maximum_owner_update_ms\": " << sample.maximum_owner_update_ms
               << ", \"snapshot_cells_copied\": " << sample.snapshot_cells_copied
               << ", \"relight_changed_chunks\": " << sample.relight_changed_chunks
               << ", \"relight_changed_cells\": " << sample.relight_changed_cells
               << ", \"collision_stale_results\": " << sample.collision_stale_results
               << ", \"relight_stale_snapshots\": " << sample.relight_stale_snapshots
               << ", \"relight_stale_results\": " << sample.relight_stale_results
               << ", \"relight_apply_budget_overruns\": " << sample.relight_apply_budget_overruns
               << '}' << (index + 1U == raw_samples.size() ? "\n" : ",\n");
    }
    output << "  ],\n";

    const auto result = summary();
    output << "  \"summary\": {\n"
           << "    \"sample_count\": " << result.sample_count << ",\n"
           << "    \"median_collision_response_ms\": " << result.median_collision_response_ms
           << ",\n"
           << "    \"p95_collision_response_ms\": " << result.p95_collision_response_ms << ",\n"
           << "    \"p99_collision_response_ms\": " << result.p99_collision_response_ms << ",\n"
           << "    \"maximum_collision_response_ms\": " << result.maximum_collision_response_ms
           << ",\n"
           << "    \"median_relight_convergence_ms\": " << result.median_relight_convergence_ms
           << ",\n"
           << "    \"p95_relight_convergence_ms\": " << result.p95_relight_convergence_ms << ",\n"
           << "    \"p99_relight_convergence_ms\": " << result.p99_relight_convergence_ms << ",\n"
           << "    \"maximum_relight_convergence_ms\": " << result.maximum_relight_convergence_ms
           << ",\n"
           << "    \"mean_owner_updates_per_edit\": " << result.mean_owner_updates_per_edit << ",\n"
           << "    \"maximum_collision_cooking_ms\": " << result.maximum_collision_cooking_ms
           << ",\n"
           << "    \"maximum_collision_apply_ms\": " << result.maximum_collision_apply_ms << ",\n"
           << "    \"maximum_relight_solve_ms\": " << result.maximum_relight_solve_ms << ",\n"
           << "    \"maximum_relight_apply_ms\": " << result.maximum_relight_apply_ms << ",\n"
           << "    \"maximum_owner_update_ms\": " << result.maximum_owner_update_ms << ",\n"
           << "    \"total_snapshot_cells_copied\": " << result.total_snapshot_cells_copied << ",\n"
           << "    \"gates\": {\"evaluated\": " << (result.gates.evaluated ? "true" : "false")
           << ", \"passed\": " << (result.gates.passed ? "true" : "false") << ", \"violations\": [";
    for (std::size_t index = 0; index < result.gates.violations.size(); ++index) {
        const auto& violation = result.gates.violations[index];
        output << (index == 0 ? "" : ", ") << "{\"metric\": ";
        write_json_string(output, violation.metric);
        output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit << '}';
    }
    output << "]}\n  }\n}\n";
    return output.str();
}

core::Status VoxelResponseBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    return status ? write_text_file(path, to_json()) : status;
}

core::Result<VoxelResponseBenchmarkReport>
run_voxel_response_benchmark(const VoxelResponseBenchmarkConfig& config) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.voxel_response.run");
    auto status = config.validate();
    if (!status) {
        return core::Result<VoxelResponseBenchmarkReport>::failure(status.error().code,
                                                                   status.error().message);
    }
    auto created = make_state(config);
    if (!created) {
        return core::Result<VoxelResponseBenchmarkReport>::failure(created.error().code,
                                                                   created.error().message);
    }
    auto state = std::move(created).value();
    status = settle_initial_state(*state, config);
    if (!status) {
        return core::Result<VoxelResponseBenchmarkReport>::failure(status.error().code,
                                                                   status.error().message);
    }

    for (std::uint32_t warmup = 0; warmup < config.warmup_repetitions; ++warmup) {
        auto sample = execute_edit(*state, config, warmup, warmup);
        if (!sample) {
            return core::Result<VoxelResponseBenchmarkReport>::failure(sample.error().code,
                                                                       sample.error().message);
        }
    }
    if (!systems_are_idle(*state)) {
        return core::Result<VoxelResponseBenchmarkReport>::failure(
            "voxel_response_benchmark.warmup_incomplete",
            "warmup edits left collision or lighting work pending");
    }
    state->collision->reset_latency_observations();
    state->lighting->reset_latency_observations();
    const auto collision_baseline = state->collision->stats();
    const auto lighting_baseline = state->lighting->stats();

    VoxelResponseBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();
    report.run.chunk_count = state->coordinates.size();
    report.run.warmup_edits = config.warmup_repetitions;
    report.run.measured_edits = config.repetitions;
    report.raw_samples.reserve(config.repetitions);
    const auto measured_start = BenchmarkClock::now();
    for (std::uint32_t repetition = 0; repetition < config.repetitions; ++repetition) {
        const auto operation = static_cast<std::uint64_t>(config.warmup_repetitions) + repetition;
        auto sample = execute_edit(*state, config, operation, repetition);
        if (!sample) {
            return core::Result<VoxelResponseBenchmarkReport>::failure(sample.error().code,
                                                                       sample.error().message);
        }
        report.run.owner_updates += sample.value().owner_updates;
        report.raw_samples.push_back(std::move(sample).value());
    }
    report.run.elapsed_us = elapsed_microseconds(measured_start, BenchmarkClock::now());

    const auto collision = state->collision->stats();
    const auto lighting = state->lighting->stats();
    report.run.collision_response_completions = collision.total_collision_response_completed;
    report.run.relight_response_completions = lighting.total_relight_response_completed;
    report.run.collision_coalesced_invalidations =
        collision.total_coalesced_collision_invalidations;
    report.run.relight_coalesced_invalidations = lighting.total_coalesced_relight_invalidations;
    report.run.collision_abandoned_invalidations =
        collision.total_abandoned_collision_invalidations;
    report.run.relight_abandoned_invalidations = lighting.total_abandoned_relight_invalidations;
    report.run.collision_stale_results = collision.stale_results - collision_baseline.stale_results;
    report.run.relight_stale_snapshots =
        lighting.stale_snapshots - lighting_baseline.stale_snapshots;
    report.run.relight_stale_results = lighting.stale_results - lighting_baseline.stale_results;
    report.run.collision_failed_results =
        collision.failed_results - collision_baseline.failed_results;
    report.run.relight_failed_results = lighting.failed_results - lighting_baseline.failed_results;
    report.run.relight_apply_budget_overruns =
        lighting.apply_budget_overruns - lighting_baseline.apply_budget_overruns;
    report.run.pending_collision_responses = collision.pending_collision_response_count;
    report.run.pending_relight_responses = lighting.pending_relight_response_count;
    const auto [current_collision, current_lighting] = current_stage_counts(*state);
    report.run.current_collision_stages = current_collision;
    report.run.current_lighting_stages = current_lighting;

    status = report.validate();
    if (!status) {
        return core::Result<VoxelResponseBenchmarkReport>::failure(status.error().code,
                                                                   status.error().message);
    }
    return core::Result<VoxelResponseBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::world::benchmark
