#include "engine/renderer/benchmark/chunk_render_readiness_benchmark.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/renderer/terrain/terrain_mapping.hpp"
#include "engine/world/meshing/chunk_mesh_snapshot.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace heartstead::renderer::benchmark {

namespace {

using BenchmarkClock = std::chrono::steady_clock;

constexpr world::ChunkCoord benchmark_center{};
constexpr rhi::RenderResourceHandle benchmark_terrain_pipeline{1};

struct PendingSample {
    ChunkRenderReadinessBenchmarkSample sample;
    bool submitted = false;
    bool published = false;
    bool mesh_resident = false;
    bool draw_eligible = false;
};

struct BenchmarkState {
    world::VoxelPalette palette;
    world::WorldState world;
    RenderCamera camera;
    std::unique_ptr<rhi::IRenderDevice> device;
    std::unique_ptr<ChunkGpuCache> cache;
    std::unique_ptr<ChunkRenderSystem> renderer;
    std::unique_ptr<world::ChunkLoadScheduler> loader;
    std::size_t baseline_live_resources = 0;
};

struct WorkloadExecution {
    ChunkRenderReadinessDeviceMetadata device;
    ChunkRenderReadinessBenchmarkRun run;
    std::vector<ChunkRenderReadinessBenchmarkSample> samples;
};

[[nodiscard]] std::uint64_t elapsed_microseconds(BenchmarkClock::time_point begin,
                                                 BenchmarkClock::time_point end) noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    return elapsed > 0 ? static_cast<std::uint64_t>(elapsed) : 0;
}

[[nodiscard]] double elapsed_milliseconds(BenchmarkClock::time_point begin,
                                          BenchmarkClock::time_point end) noexcept {
    return std::max(0.0, std::chrono::duration<double, std::milli>(end - begin).count());
}

[[nodiscard]] std::uint64_t mix_hash(std::uint64_t value) noexcept {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] std::uint64_t coordinate_hash(world::ChunkCoord coord, std::uint64_t seed) noexcept {
    auto value = mix_hash(static_cast<std::uint64_t>(coord.x) ^ seed);
    value ^= mix_hash(static_cast<std::uint64_t>(coord.y) + 0x9e3779b97f4a7c15ULL);
    value ^= mix_hash(static_cast<std::uint64_t>(coord.z) + 0xd1b54a32d192ed03ULL);
    return mix_hash(value);
}

[[nodiscard]] std::vector<world::ChunkCoord> benchmark_coordinates(std::uint16_t radius) {
    std::vector<world::ChunkCoord> result;
    const auto signed_radius = static_cast<std::int64_t>(radius);
    const auto radius_squared = signed_radius * signed_radius;
    for (std::int64_t z = -signed_radius; z <= signed_radius; ++z) {
        for (std::int64_t x = -signed_radius; x <= signed_radius; ++x) {
            if (x * x + z * z <= radius_squared) {
                result.push_back(
                    {benchmark_center.x + x, benchmark_center.y, benchmark_center.z + z});
            }
        }
    }
    std::ranges::sort(result, [](world::ChunkCoord left, world::ChunkCoord right) {
        const auto left_distance = left.x * left.x + left.z * left.z;
        const auto right_distance = right.x * right.x + right.z * right.z;
        return left_distance != right_distance ? left_distance < right_distance : left < right;
    });
    return result;
}

class BenchmarkChunkGenerator final : public world::IChunkLoadGenerator {
  public:
    explicit BenchmarkChunkGenerator(std::uint64_t seed) noexcept : seed_(seed) {}

    [[nodiscard]] core::Result<world::VoxelChunk> generate(world::ChunkCoord coord) const override {
        constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
        const auto height = static_cast<std::size_t>(coordinate_hash(coord, seed_) % 3U) + 1U;
        std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells,
                                            world::VoxelCell::air());
        for (std::size_t z = 0; z < edge; ++z) {
            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < edge; ++x) {
                    cells[z * edge * edge + y * edge + x] = world::VoxelCell{1, 0};
                }
            }
        }
        world::VoxelChunk chunk(coord);
        auto status = chunk.load_generated_cells(std::move(cells));
        if (!status) {
            return core::Result<world::VoxelChunk>::failure(status.error().code,
                                                            status.error().message);
        }
        return core::Result<world::VoxelChunk>::success(std::move(chunk));
    }

  private:
    std::uint64_t seed_ = 0;
};

[[nodiscard]] core::Result<world::VoxelPalette> make_palette() {
    const auto prototype = core::PrototypeId::parse("benchmark:voxels/render_readiness_stone");
    if (!prototype) {
        return core::Result<world::VoxelPalette>::failure(
            "chunk_render_readiness_benchmark.invalid_voxel_id",
            "the built-in render-readiness voxel id is invalid");
    }
    world::VoxelDefinition stone;
    stone.type = 1;
    stone.prototype_id = *prototype;
    stone.display_name = "Render-readiness benchmark stone";
    stone.terrain_material = "benchmark_stone";
    stone.mining_tool = "pickaxe";

    world::VoxelPalette palette;
    auto status = palette.add(std::move(stone));
    if (!status) {
        return core::Result<world::VoxelPalette>::failure(status.error().code,
                                                          status.error().message);
    }
    return core::Result<world::VoxelPalette>::success(std::move(palette));
}

[[nodiscard]] core::Result<RenderCamera> make_camera(std::uint16_t radius) {
    auto center = world::chunk_local_to_block(benchmark_center, {16, 0, 16});
    if (!center) {
        return core::Result<RenderCamera>::failure(center.error().code, center.error().message);
    }
    constexpr auto edge = static_cast<std::int64_t>(world::VoxelChunk::edge_length);
    RenderCamera camera;
    camera.floating_origin.block = center.value();
    camera.floating_origin.block.z += (static_cast<std::int64_t>(radius) + 3) * edge;
    camera.local_position = {0.0F, 24.0F, 0.0F};
    camera.pitch_radians = -0.20F;
    camera.aspect_ratio = 2.0F;
    camera.far_plane = 2'000.0F;
    auto status = camera.update_matrices();
    if (!status) {
        return core::Result<RenderCamera>::failure(status.error().code, status.error().message);
    }
    return core::Result<RenderCamera>::success(camera);
}

[[nodiscard]] ChunkRenderReadinessDeviceMetadata device_metadata(const rhi::IRenderDevice& device) {
    const auto info = device.info();
    const auto capabilities = device.capabilities();
    ChunkRenderReadinessDeviceMetadata result;
    result.backend = device.backend_name();
    result.device_name = info.device_name;
    result.driver_name = info.driver_name;
    result.driver_info = info.driver_info;
    result.vendor_id = info.vendor_id;
    result.device_id = info.device_id;
    result.api_version = info.api_version;
    result.driver_version = info.driver_version;
    result.headless = capabilities.headless;
    return result;
}

[[nodiscard]] core::Result<std::unique_ptr<BenchmarkState>>
make_state(const ChunkRenderReadinessBenchmarkConfig& config) {
    auto state = std::make_unique<BenchmarkState>();
    auto palette = make_palette();
    if (!palette) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(palette.error().code,
                                                                      palette.error().message);
    }
    state->palette = std::move(palette).value();
    auto camera = make_camera(config.horizontal_radius_chunks);
    if (!camera) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(camera.error().code,
                                                                      camera.error().message);
    }
    state->camera = camera.value();

    rhi::RenderDeviceDesc device_desc;
    device_desc.backend = config.render_backend;
    device_desc.application_name = "Heartstead Chunk Render Readiness Benchmark";
    device_desc.initial_extent = {1280, 640};
    device_desc.enable_validation = false;
    auto device = rhi::create_render_device(device_desc);
    if (!device) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(device.error().code,
                                                                      device.error().message);
    }
    state->device = std::move(device).value();
    state->baseline_live_resources = state->device->live_resource_count();
    state->cache = std::make_unique<ChunkGpuCache>(*state->device);
    auto status = state->cache->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                      status.error().message);
    }
    state->renderer = std::make_unique<ChunkRenderSystem>(*state->cache, benchmark_terrain_pipeline,
                                                          &state->palette, config.rendering);
    status = state->renderer->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                      status.error().message);
    }

    world::ChunkLoadSchedulerContext context;
    context.generator = std::make_shared<BenchmarkChunkGenerator>(config.seed);
    auto loader = world::ChunkLoadScheduler::create(std::move(context), config.loading);
    if (!loader) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(loader.error().code,
                                                                      loader.error().message);
    }
    state->loader = std::move(loader).value();
    return core::Result<std::unique_ptr<BenchmarkState>>::success(std::move(state));
}

void discard_unowned_dirty_regions(world::WorldState& world) {
    static_cast<void>(world.dirty_regions().consume_kind(dirty::DirtyRegionKind::chunk_collision));
    static_cast<void>(world.dirty_regions().consume_kind(dirty::DirtyRegionKind::chunk_lighting));
}

[[nodiscard]] bool exact_mesh_is_resident(const BenchmarkState& state,
                                          world::ChunkIdentity identity) {
    const auto* chunk = state.world.chunks().find(identity.coordinate);
    const auto* entry = state.cache->find(identity);
    if (chunk == nullptr || entry == nullptr || chunk->identity() != identity ||
        entry->state != ChunkGpuState::resident ||
        entry->resident_content_revision != chunk->content_revision() ||
        entry->resident_render_table_revision != state.palette.render_revision() ||
        !chunk->stages().record(world::ChunkStage::mesh).resident_is_current()) {
        return false;
    }
    return world::dependency_revisions_match(state.world.chunks(),
                                             entry->resident_dependency_revisions);
}

[[nodiscard]] const world::ChunkLoadTimingSample*
find_success_timing(const world::ChunkLoadPublicationReport& report,
                    world::ChunkLoadRequestId request_id) noexcept {
    const auto found = std::ranges::find_if(report.timings, [request_id](const auto& timing) {
        return timing.request_id == request_id &&
               timing.state == world::ChunkLoadResultState::succeeded;
    });
    return found == report.timings.end() ? nullptr : &*found;
}

[[nodiscard]] core::Status
submit_available(BenchmarkState& state, std::span<const world::ChunkCoord> coordinates,
                 std::vector<PendingSample>& samples, std::size_t& next_submission,
                 ChunkRenderReadinessBenchmarkRun& run, bool count_deferral) {
    while (next_submission < coordinates.size() && state.loader->has_capacity()) {
        auto submitted =
            state.loader->submit(coordinates[next_submission], jobs::JobPriority::high);
        if (!submitted) {
            return core::Status::failure(submitted.error().code, submitted.error().message);
        }
        auto& sample = samples[next_submission];
        if (sample.submitted || sample.sample.request_id != 0) {
            return core::Status::failure(
                "chunk_render_readiness_benchmark.duplicate_submission",
                "one required chunk was submitted to the load scheduler more than once");
        }
        sample.submitted = true;
        sample.sample.request_id = submitted.value().value();
        ++next_submission;
    }
    if (count_deferral && next_submission < coordinates.size() && !state.loader->has_capacity()) {
        ++run.admission_deferred_updates;
    }
    return core::Status::ok();
}

void pace_owner_thread(BenchmarkClock::time_point& next_update, BenchmarkClock::time_point deadline,
                       std::uint64_t update_interval_us) {
    next_update += std::chrono::microseconds(update_interval_us);
    std::this_thread::sleep_until(std::min(next_update, deadline));
}

[[nodiscard]] core::Status shutdown_state(BenchmarkState& state,
                                          ChunkRenderReadinessBenchmarkRun& run) {
    state.loader->shutdown();
    state.renderer->shutdown();
    auto status = state.device->wait_idle();
    if (!status) {
        return status;
    }
    status = state.cache->shutdown();
    if (!status) {
        return status;
    }
    run.baseline_live_render_resources = state.baseline_live_resources;
    run.final_live_render_resources = state.device->live_resource_count();
    if (run.final_live_render_resources != run.baseline_live_render_resources) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.render_resource_leak",
            "render-readiness workload did not release every render resource it created");
    }
    return core::Status::ok();
}

[[nodiscard]] core::Result<WorkloadExecution>
run_workload(const ChunkRenderReadinessBenchmarkConfig& config, std::uint32_t repetition) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.chunk_render_readiness.workload");
    auto created = make_state(config);
    if (!created) {
        return core::Result<WorkloadExecution>::failure(created.error().code,
                                                        created.error().message);
    }
    auto state = std::move(created).value();
    WorkloadExecution execution;
    execution.device = device_metadata(*state->device);
    execution.run.repetition = repetition;

    const auto coordinates = benchmark_coordinates(config.horizontal_radius_chunks);
    execution.run.desired_chunks = coordinates.size();
    std::vector<PendingSample> pending(coordinates.size());
    std::map<world::ChunkCoord, std::size_t> sample_by_coord;
    std::set<std::uint32_t> expected_draw_seeds;
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        pending[index].sample.repetition = repetition;
        pending[index].sample.ordinal = static_cast<std::uint32_t>(index);
        pending[index].sample.coord = coordinates[index];
        sample_by_coord.emplace(coordinates[index], index);
        if (!expected_draw_seeds.insert(terrain::terrain_coordinate_key(coordinates[index]))
                 .second) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_render_readiness_benchmark.draw_identity_collision",
                "the benchmark corpus contains colliding terrain draw identity seeds");
        }
    }

    // Every coordinate becomes immediately required at this one timestamp. Deferred admission
    // retains the original start, preventing coordinated omission under load-queue saturation.
    const auto interest_started_at = BenchmarkClock::now();
    const auto deadline = interest_started_at + std::chrono::milliseconds(config.timeout_ms);
    auto next_update = interest_started_at;
    std::size_t next_submission = 0;
    auto status =
        submit_available(*state, coordinates, pending, next_submission, execution.run, false);
    if (!status) {
        return core::Result<WorkloadExecution>::failure(status.error().code,
                                                        status.error().message);
    }

    std::size_t published_count = 0;
    std::size_t draw_eligible_count = 0;
    while (true) {
        if (BenchmarkClock::now() >= deadline) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_render_readiness_benchmark.timeout",
                "required chunks did not converge to exact draw eligibility before the timeout");
        }
        const auto owner_update_started_at = BenchmarkClock::now();
        auto publication = state->loader->update(state->world);
        const auto publication_finished_at = BenchmarkClock::now();
        ++execution.run.owner_updates;
        execution.run.load_item_budget_exhaustions += publication.item_budget_exhausted ? 1U : 0U;
        execution.run.load_time_budget_exhaustions += publication.time_budget_exhausted ? 1U : 0U;
        execution.run.maximum_publication_time_us =
            std::max(execution.run.maximum_publication_time_us, publication.publication_time_us);
        if (!publication.failures.empty()) {
            const auto& failure = publication.failures.front();
            return core::Result<WorkloadExecution>::failure(failure.error.code,
                                                            failure.error.message);
        }
        if (!publication.cancelled.empty() || !publication.stale.empty()) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_render_readiness_benchmark.invalid_load_lifecycle",
                "a fixed required-interest workload produced a cancelled or stale chunk load");
        }

        for (const auto& load : publication.published) {
            const auto found = sample_by_coord.find(load.coord);
            if (found == sample_by_coord.end()) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_render_readiness_benchmark.off_interest_publication",
                    "the loader published a chunk outside the fixed required-interest set");
            }
            auto& sample = pending[found->second];
            if (!sample.submitted || sample.published || !load.identity.is_valid()) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_render_readiness_benchmark.duplicate_publication",
                    "a required chunk publication was invalid or occurred more than once");
            }
            const auto request_id = world::ChunkLoadRequestId::from_value(sample.sample.request_id);
            const auto* timing = find_success_timing(publication, request_id);
            if (timing == nullptr || timing->coord != load.coord ||
                timing->source != world::ChunkStreamLoadSource::generated ||
                timing->saved_edit_count != 0 || load.source != timing->source ||
                load.saved_edit_count != 0) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_render_readiness_benchmark.invalid_publication_timing",
                    "a required publication has no matching generated-load lifecycle timing");
            }
            sample.published = true;
            sample.sample.load_generation = load.identity.load_generation;
            sample.sample.source = load.source;
            sample.sample.saved_edit_count = load.saved_edit_count;
            sample.sample.interest_to_publication_us =
                elapsed_microseconds(interest_started_at, publication_finished_at);
            sample.sample.scheduler_pipeline_ms = timing->pipeline_latency_ms;
            ++published_count;
        }

        status = state->renderer->process_chunk_loads(publication.published);
        if (!status) {
            return core::Result<WorkloadExecution>::failure(status.error().code,
                                                            status.error().message);
        }
        discard_unowned_dirty_regions(state->world);

        const auto synchronize_started_at = BenchmarkClock::now();
        status = state->renderer->synchronize(state->world, state->camera);
        const auto synchronize_finished_at = BenchmarkClock::now();
        if (!status) {
            return core::Result<WorkloadExecution>::failure(status.error().code,
                                                            status.error().message);
        }
        execution.run.maximum_render_synchronize_ms =
            std::max(execution.run.maximum_render_synchronize_ms,
                     elapsed_milliseconds(synchronize_started_at, synchronize_finished_at));

        for (auto& pending_sample : pending) {
            if (!pending_sample.published || pending_sample.mesh_resident) {
                continue;
            }
            const world::ChunkIdentity identity{pending_sample.sample.coord,
                                                pending_sample.sample.load_generation};
            if (!exact_mesh_is_resident(*state, identity)) {
                continue;
            }
            const auto* entry = state->cache->find(identity);
            const auto* chunk = state->world.chunks().find(identity.coordinate);
            if (entry == nullptr || chunk == nullptr || !entry->has_drawable_mesh()) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_render_readiness_benchmark.empty_required_mesh",
                    "the non-empty benchmark corpus produced an empty resident chunk mesh");
            }
            pending_sample.mesh_resident = true;
            pending_sample.sample.interest_to_mesh_resident_us =
                elapsed_microseconds(interest_started_at, synchronize_finished_at);
            pending_sample.sample.publication_to_mesh_resident_us =
                pending_sample.sample.interest_to_mesh_resident_us -
                pending_sample.sample.interest_to_publication_us;
            pending_sample.sample.mesh_request_revision =
                chunk->stages().record(world::ChunkStage::mesh).resident_request_revision;
        }

        const auto draw_list_started_at = BenchmarkClock::now();
        auto draws = state->renderer->build_draw_list(state->camera);
        const auto draw_list_finished_at = BenchmarkClock::now();
        const auto measured_draw_list_ms =
            elapsed_milliseconds(draw_list_started_at, draw_list_finished_at);
        std::map<std::uint32_t, std::uint32_t> draw_count_by_seed;
        for (const auto& draw : draws.draws) {
            if (!expected_draw_seeds.contains(draw.texture_variation_seed)) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_render_readiness_benchmark.unknown_draw_command",
                    "the chunk draw list contains geometry outside the required benchmark set");
            }
            ++draw_count_by_seed[draw.texture_variation_seed];
        }

        for (auto& pending_sample : pending) {
            if (!pending_sample.mesh_resident || pending_sample.draw_eligible) {
                continue;
            }
            const world::ChunkIdentity identity{pending_sample.sample.coord,
                                                pending_sample.sample.load_generation};
            const auto seed = terrain::terrain_coordinate_key(identity.coordinate);
            const auto found = draw_count_by_seed.find(seed);
            if (found == draw_count_by_seed.end() || !exact_mesh_is_resident(*state, identity)) {
                continue;
            }
            const auto* entry = state->cache->find(identity);
            const auto* chunk = state->world.chunks().find(identity.coordinate);
            if (entry == nullptr || chunk == nullptr || !entry->has_drawable_mesh() ||
                chunk->stages().record(world::ChunkStage::mesh).resident_request_revision !=
                    pending_sample.sample.mesh_request_revision) {
                return core::Result<WorkloadExecution>::failure(
                    "chunk_render_readiness_benchmark.draw_revision_mismatch",
                    "the emitted draw does not reference the exact mesh observed this update");
            }
            pending_sample.draw_eligible = true;
            pending_sample.sample.interest_to_draw_eligibility_us =
                elapsed_microseconds(interest_started_at, draw_list_finished_at);
            pending_sample.sample.mesh_resident_to_draw_eligibility_us =
                pending_sample.sample.interest_to_draw_eligibility_us -
                pending_sample.sample.interest_to_mesh_resident_us;
            pending_sample.sample.first_draw_owner_update = execution.run.owner_updates;
            pending_sample.sample.draw_command_count = found->second;
            pending_sample.sample.vertex_count = entry->mesh.vertex_count;
            pending_sample.sample.index_count = entry->mesh.index_count;
            pending_sample.sample.resident_bytes = entry->resident_bytes();
            ++draw_eligible_count;
        }

        const auto& render_stats = state->renderer->stats();
        execution.run.scheduled_mesh_jobs += render_stats.scheduled_mesh_count;
        execution.run.cancelled_mesh_results += render_stats.cancelled_mesh_count;
        execution.run.uploaded_chunks += render_stats.uploaded_chunk_count;
        execution.run.uploaded_bytes += render_stats.uploaded_bytes;
        execution.run.maximum_snapshot_cells_per_update = std::max(
            execution.run.maximum_snapshot_cells_per_update, render_stats.snapshot_cells_copied);
        execution.run.maximum_uploaded_bytes_per_update =
            std::max(execution.run.maximum_uploaded_bytes_per_update, render_stats.uploaded_bytes);
        execution.run.peak_gpu_resident_bytes =
            std::max(execution.run.peak_gpu_resident_bytes, render_stats.cache.resident_bytes);
        execution.run.maximum_draw_list_build_ms =
            std::max(execution.run.maximum_draw_list_build_ms,
                     std::max(measured_draw_list_ms, render_stats.draw_list_ms));
        execution.run.maximum_chunk_snapshot_ms =
            std::max(execution.run.maximum_chunk_snapshot_ms, render_stats.chunk_snapshot_ms);
        execution.run.maximum_meshing_ms =
            std::max(execution.run.maximum_meshing_ms, render_stats.meshing_ms);
        execution.run.maximum_upload_preparation_ms = std::max(
            execution.run.maximum_upload_preparation_ms, render_stats.upload_preparation_ms);
        execution.run.maximum_upload_ms =
            std::max(execution.run.maximum_upload_ms, render_stats.upload_ms);
        execution.run.maximum_gpu_wait_ms =
            std::max(execution.run.maximum_gpu_wait_ms, render_stats.gpu_wait_ms);

        status =
            submit_available(*state, coordinates, pending, next_submission, execution.run, true);
        if (!status) {
            return core::Result<WorkloadExecution>::failure(status.error().code,
                                                            status.error().message);
        }

        std::size_t current_mesh_stages = 0;
        std::size_t exact_resident_meshes = 0;
        std::size_t drawable_resident_meshes = 0;
        std::size_t exact_draws_this_update = 0;
        for (const auto& pending_sample : pending) {
            if (!pending_sample.published) {
                continue;
            }
            const world::ChunkIdentity identity{pending_sample.sample.coord,
                                                pending_sample.sample.load_generation};
            const auto* chunk = state->world.chunks().find(identity.coordinate);
            const auto* entry = state->cache->find(identity);
            if (chunk != nullptr && chunk->identity() == identity &&
                chunk->stages().record(world::ChunkStage::mesh).resident_is_current()) {
                ++current_mesh_stages;
            }
            if (exact_mesh_is_resident(*state, identity)) {
                ++exact_resident_meshes;
                if (entry != nullptr && entry->has_drawable_mesh()) {
                    ++drawable_resident_meshes;
                    exact_draws_this_update +=
                        draw_count_by_seed.contains(
                            terrain::terrain_coordinate_key(identity.coordinate))
                            ? 1U
                            : 0U;
                }
            }
        }

        const auto loader_idle = next_submission == coordinates.size() &&
                                 !state->loader->has_in_flight() &&
                                 published_count == coordinates.size();
        const auto renderer_idle = render_stats.pending_mesh_count == 0 &&
                                   render_stats.in_flight_mesh_count == 0 &&
                                   render_stats.pending_upload_count == 0 &&
                                   render_stats.pending_edit_to_visible_count == 0;
        const auto final_exact = current_mesh_stages == coordinates.size() &&
                                 exact_resident_meshes == coordinates.size() &&
                                 drawable_resident_meshes == coordinates.size() &&
                                 exact_draws_this_update == coordinates.size() &&
                                 draws.drawn_chunk_count == coordinates.size();

        execution.run.maximum_owner_update_ms =
            std::max(execution.run.maximum_owner_update_ms,
                     elapsed_milliseconds(owner_update_started_at, BenchmarkClock::now()));
        if (loader_idle && renderer_idle && final_exact &&
            draw_eligible_count == coordinates.size()) {
            execution.run.current_mesh_stages = current_mesh_stages;
            execution.run.exact_resident_meshes = exact_resident_meshes;
            execution.run.drawable_resident_meshes = drawable_resident_meshes;
            break;
        }
        pace_owner_thread(next_update, deadline, config.update_interval_us);
    }

    execution.run.elapsed_us = elapsed_microseconds(interest_started_at, BenchmarkClock::now());
    const auto& load_stats = state->loader->stats();
    const auto& render_stats = state->renderer->stats();
    execution.run.submitted_requests = load_stats.submitted_requests;
    execution.run.published_requests = load_stats.published_requests;
    execution.run.draw_eligible_chunks = draw_eligible_count;
    execution.run.cancelled_requests = load_stats.cancelled_requests;
    execution.run.stale_requests = load_stats.stale_requests;
    execution.run.failed_requests = load_stats.failed_requests;
    execution.run.rejected_requests = load_stats.rejected_requests;
    execution.run.maximum_publication_time_us =
        std::max(execution.run.maximum_publication_time_us, load_stats.maximum_publication_time_us);
    execution.run.reserved_working_bytes_high_water = load_stats.reserved_working_bytes_high_water;
    execution.run.final_reserved_working_bytes = load_stats.reserved_working_bytes;
    execution.run.completed_mesh_jobs = render_stats.total_completed_mesh_job_count;
    execution.run.built_meshes = render_stats.total_built_mesh_count;
    execution.run.published_meshes = render_stats.total_published_mesh_count;
    execution.run.stale_mesh_results = render_stats.total_stale_mesh_result_count;
    execution.run.failed_mesh_results = render_stats.total_failed_mesh_count;
    execution.run.failed_uploads = render_stats.total_failed_upload_count;
    execution.run.coalesced_mesh_invalidations =
        render_stats.total_coalesced_edit_invalidation_count;
    execution.run.abandoned_mesh_invalidations =
        render_stats.total_abandoned_edit_invalidation_count;
    execution.run.mesh_builds_per_publication = render_stats.mesh_builds_per_publication;
    execution.run.pending_meshes = render_stats.pending_mesh_count;
    execution.run.in_flight_meshes = render_stats.in_flight_mesh_count;
    execution.run.pending_uploads = render_stats.pending_upload_count;
    execution.run.pending_edit_responses = render_stats.pending_edit_to_visible_count;

    execution.samples.reserve(pending.size());
    for (auto& sample : pending) {
        if (!sample.submitted || !sample.published || !sample.mesh_resident ||
            !sample.draw_eligible) {
            return core::Result<WorkloadExecution>::failure(
                "chunk_render_readiness_benchmark.censored_sample",
                "the workload ended without every required chunk lifecycle observation");
        }
        execution.samples.push_back(std::move(sample.sample));
    }
    status = shutdown_state(*state, execution.run);
    if (!status) {
        return core::Result<WorkloadExecution>::failure(status.error().code,
                                                        status.error().message);
    }
    return core::Result<WorkloadExecution>::success(std::move(execution));
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
    const auto string_field = [&output](std::string_view name, std::string_view value) {
        output << "    \"" << name << "\": ";
        write_json_string(output, value);
        output << ",\n";
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
            return core::Status::failure("chunk_render_readiness_benchmark.create_directory_failed",
                                         "failed to create benchmark output directory: " +
                                             error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("chunk_render_readiness_benchmark.open_output_failed",
                                     "failed to open benchmark output: " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        return core::Status::failure("chunk_render_readiness_benchmark.write_output_failed",
                                     "failed to write benchmark output: " + path.string());
    }
    return core::Status::ok();
}

} // namespace

core::Status ChunkRenderReadinessBenchmarkConfig::validate() const {
    if (render_backend != rhi::RenderBackend::headless &&
        render_backend != rhi::RenderBackend::vulkan) {
        return core::Status::failure("chunk_render_readiness_benchmark.invalid_backend",
                                     "render backend must be headless or Vulkan");
    }
    if (horizontal_radius_chunks > 8 ||
        horizontal_radius_chunks > rendering.distances.visible_horizontal_radius) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.invalid_radius",
            "required radius must be at most eight chunks and fit the visible render radius");
    }
    if (repetitions == 0 || repetitions > 100 || warmup_repetitions > 100) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.invalid_repetitions",
            "render-readiness repetitions must be 1..100 and warmups 0..100");
    }
    if (update_interval_us == 0 || update_interval_us > 1'000'000 || timeout_ms == 0 ||
        timeout_ms > 600'000) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.invalid_timing",
            "owner cadence and workload timeout must be positive and bounded");
    }
    if (!std::isfinite(maximum_draw_eligibility_p95_ms) || maximum_draw_eligibility_p95_ms <= 0.0 ||
        !std::isfinite(maximum_upload_preparation_ms) || maximum_upload_preparation_ms <= 0.0 ||
        !std::isfinite(maximum_synchronous_gpu_wait_ms) || maximum_synchronous_gpu_wait_ms < 0.0 ||
        !std::isfinite(maximum_mesh_builds_per_publication) ||
        maximum_mesh_builds_per_publication < 1.0) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.invalid_gates",
            "draw, upload-preparation, and synchronous-wait gates must be finite and bounded");
    }
    auto status = loading.validate();
    return status ? rendering.validate() : status;
}

core::Status ChunkRenderReadinessBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    const auto coordinates = benchmark_coordinates(config.horizontal_radius_chunks);
    const auto expected_run_count = static_cast<std::size_t>(config.repetitions);
    if (runs.size() != expected_run_count ||
        raw_samples.size() != expected_run_count * coordinates.size()) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.incomplete_report",
            "render-readiness report does not contain every configured run and chunk sample");
    }
    if (device.backend != rhi::render_backend_name(config.render_backend) ||
        device.device_name.empty() || device.driver_name.empty()) {
        return core::Status::failure(
            "chunk_render_readiness_benchmark.invalid_device_metadata",
            "render-readiness report has incomplete or inconsistent device provenance");
    }

    std::set<std::uint32_t> run_keys;
    for (const auto& run : runs) {
        if (run.repetition >= config.repetitions || !run_keys.insert(run.repetition).second ||
            run.desired_chunks != coordinates.size() ||
            run.submitted_requests != coordinates.size() ||
            run.published_requests != coordinates.size() ||
            run.draw_eligible_chunks != coordinates.size() || run.cancelled_requests != 0 ||
            run.stale_requests != 0 || run.failed_requests != 0 || run.rejected_requests != 0 ||
            run.final_reserved_working_bytes != 0 || run.failed_mesh_results != 0 ||
            run.failed_uploads != 0 || run.abandoned_mesh_invalidations != 0 ||
            run.pending_meshes != 0 || run.in_flight_meshes != 0 || run.pending_uploads != 0 ||
            run.pending_edit_responses != 0 || run.current_mesh_stages != coordinates.size() ||
            run.exact_resident_meshes != coordinates.size() ||
            run.drawable_resident_meshes != coordinates.size() ||
            run.baseline_live_render_resources != run.final_live_render_resources ||
            run.published_meshes < coordinates.size() || run.uploaded_chunks < coordinates.size() ||
            run.uploaded_bytes == 0 || !std::isfinite(run.mesh_builds_per_publication) ||
            run.mesh_builds_per_publication < 1.0 ||
            run.maximum_snapshot_cells_per_update > config.rendering.max_snapshot_cells_per_frame ||
            run.maximum_uploaded_bytes_per_update > config.rendering.max_bytes_uploaded_per_frame ||
            run.peak_gpu_resident_bytes > config.rendering.gpu_terrain_budget_bytes) {
            return core::Status::failure(
                "chunk_render_readiness_benchmark.invalid_run",
                "a render-readiness run did not converge cleanly within its declared budgets");
        }
    }

    const std::set<world::ChunkCoord> coordinate_set(coordinates.begin(), coordinates.end());
    std::set<std::tuple<std::uint32_t, world::ChunkCoord>> sample_keys;
    for (const auto& sample : raw_samples) {
        if (sample.repetition >= config.repetitions || !run_keys.contains(sample.repetition) ||
            sample.ordinal >= coordinates.size() || !coordinate_set.contains(sample.coord) ||
            sample.request_id == 0 || sample.load_generation == 0 ||
            sample.mesh_request_revision == 0 ||
            sample.source != world::ChunkStreamLoadSource::generated ||
            sample.saved_edit_count != 0 || sample.interest_to_publication_us == 0 ||
            sample.interest_to_mesh_resident_us < sample.interest_to_publication_us ||
            sample.interest_to_draw_eligibility_us < sample.interest_to_mesh_resident_us ||
            sample.publication_to_mesh_resident_us !=
                sample.interest_to_mesh_resident_us - sample.interest_to_publication_us ||
            sample.mesh_resident_to_draw_eligibility_us !=
                sample.interest_to_draw_eligibility_us - sample.interest_to_mesh_resident_us ||
            !std::isfinite(sample.scheduler_pipeline_ms) || sample.scheduler_pipeline_ms < 0.0 ||
            sample.first_draw_owner_update == 0 || sample.draw_command_count == 0 ||
            sample.vertex_count == 0 || sample.index_count == 0 || sample.resident_bytes == 0 ||
            !sample_keys.insert({sample.repetition, sample.coord}).second) {
            return core::Status::failure(
                "chunk_render_readiness_benchmark.invalid_sample",
                "render-readiness report contains an invalid, duplicate, or censored sample");
        }
        if (coordinates[sample.ordinal] != sample.coord) {
            return core::Status::failure(
                "chunk_render_readiness_benchmark.invalid_sample_ordinal",
                "render-readiness sample ordinal does not match deterministic interest order");
        }
    }
    return core::Status::ok();
}

ChunkRenderReadinessBenchmarkSummary ChunkRenderReadinessBenchmarkReport::summary() const {
    ChunkRenderReadinessBenchmarkSummary result;
    result.run_count = runs.size();
    result.sample_count = raw_samples.size();
    std::vector<double> publication_ms;
    std::vector<double> publication_to_mesh_ms;
    std::vector<double> interest_to_mesh_ms;
    std::vector<double> draw_ms;
    publication_ms.reserve(raw_samples.size());
    publication_to_mesh_ms.reserve(raw_samples.size());
    interest_to_mesh_ms.reserve(raw_samples.size());
    draw_ms.reserve(raw_samples.size());
    for (const auto& sample : raw_samples) {
        publication_ms.push_back(static_cast<double>(sample.interest_to_publication_us) / 1'000.0);
        publication_to_mesh_ms.push_back(
            static_cast<double>(sample.publication_to_mesh_resident_us) / 1'000.0);
        interest_to_mesh_ms.push_back(static_cast<double>(sample.interest_to_mesh_resident_us) /
                                      1'000.0);
        draw_ms.push_back(static_cast<double>(sample.interest_to_draw_eligibility_us) / 1'000.0);
    }
    std::ranges::sort(publication_ms);
    std::ranges::sort(publication_to_mesh_ms);
    std::ranges::sort(interest_to_mesh_ms);
    std::ranges::sort(draw_ms);
    result.median_interest_to_publication_ms = percentile(publication_ms, 0.50);
    result.p95_interest_to_publication_ms = percentile(publication_ms, 0.95);
    result.p99_interest_to_publication_ms = percentile(publication_ms, 0.99);
    result.median_publication_to_mesh_resident_ms = percentile(publication_to_mesh_ms, 0.50);
    result.p95_publication_to_mesh_resident_ms = percentile(publication_to_mesh_ms, 0.95);
    result.p99_publication_to_mesh_resident_ms = percentile(publication_to_mesh_ms, 0.99);
    result.median_interest_to_mesh_resident_ms = percentile(interest_to_mesh_ms, 0.50);
    result.p95_interest_to_mesh_resident_ms = percentile(interest_to_mesh_ms, 0.95);
    result.p99_interest_to_mesh_resident_ms = percentile(interest_to_mesh_ms, 0.99);
    result.median_interest_to_draw_eligibility_ms = percentile(draw_ms, 0.50);
    result.p95_interest_to_draw_eligibility_ms = percentile(draw_ms, 0.95);
    result.p99_interest_to_draw_eligibility_ms = percentile(draw_ms, 0.99);
    result.maximum_interest_to_draw_eligibility_ms = draw_ms.empty() ? 0.0 : draw_ms.back();

    double throughput_total = 0.0;
    for (const auto& run : runs) {
        if (run.elapsed_us != 0) {
            throughput_total += static_cast<double>(run.desired_chunks) * 1'000'000.0 /
                                static_cast<double>(run.elapsed_us);
        }
        result.maximum_publication_time_us =
            std::max(result.maximum_publication_time_us, run.maximum_publication_time_us);
        result.reserved_working_bytes_high_water = std::max(
            result.reserved_working_bytes_high_water, run.reserved_working_bytes_high_water);
        result.peak_gpu_resident_bytes =
            std::max(result.peak_gpu_resident_bytes, run.peak_gpu_resident_bytes);
        result.maximum_snapshot_cells_per_update = std::max(
            result.maximum_snapshot_cells_per_update, run.maximum_snapshot_cells_per_update);
        result.maximum_uploaded_bytes_per_update = std::max(
            result.maximum_uploaded_bytes_per_update, run.maximum_uploaded_bytes_per_update);
        result.maximum_mesh_builds_per_publication =
            std::max(result.maximum_mesh_builds_per_publication, run.mesh_builds_per_publication);
        result.maximum_owner_update_ms =
            std::max(result.maximum_owner_update_ms, run.maximum_owner_update_ms);
        result.maximum_render_synchronize_ms =
            std::max(result.maximum_render_synchronize_ms, run.maximum_render_synchronize_ms);
        result.maximum_draw_list_build_ms =
            std::max(result.maximum_draw_list_build_ms, run.maximum_draw_list_build_ms);
        result.maximum_chunk_snapshot_ms =
            std::max(result.maximum_chunk_snapshot_ms, run.maximum_chunk_snapshot_ms);
        result.maximum_meshing_ms = std::max(result.maximum_meshing_ms, run.maximum_meshing_ms);
        result.maximum_upload_preparation_ms =
            std::max(result.maximum_upload_preparation_ms, run.maximum_upload_preparation_ms);
        result.maximum_upload_ms = std::max(result.maximum_upload_ms, run.maximum_upload_ms);
        result.maximum_gpu_wait_ms = std::max(result.maximum_gpu_wait_ms, run.maximum_gpu_wait_ms);
        result.total_stale_mesh_results += run.stale_mesh_results;
        result.total_cancelled_mesh_results += run.cancelled_mesh_results;
    }
    result.mean_chunks_per_second =
        runs.empty() ? 0.0 : throughput_total / static_cast<double>(runs.size());

    result.gates.evaluated = config.enforce_gates;
    if (config.enforce_gates) {
        const auto check = [&result](std::string metric, double actual, double limit) {
            if (!std::isfinite(actual) || actual > limit) {
                result.gates.violations.push_back({std::move(metric), actual, limit});
            }
        };
        check("p95_interest_to_draw_eligibility_ms", result.p95_interest_to_draw_eligibility_ms,
              config.maximum_draw_eligibility_p95_ms);
        check("maximum_upload_preparation_ms", result.maximum_upload_preparation_ms,
              config.maximum_upload_preparation_ms);
        check("maximum_synchronous_gpu_wait_ms", result.maximum_gpu_wait_ms,
              config.maximum_synchronous_gpu_wait_ms);
        check("maximum_mesh_builds_per_publication", result.maximum_mesh_builds_per_publication,
              config.maximum_mesh_builds_per_publication);
        result.gates.passed = result.gates.violations.empty();
    }
    return result;
}

bool ChunkRenderReadinessBenchmarkReport::gates_passed() const {
    return summary().gates.passed;
}

std::string ChunkRenderReadinessBenchmarkReport::to_json() const {
    std::ostringstream output;
    output << std::setprecision(17);
    output << "{\n"
           << "  \"schema_version\": " << schema_version << ",\n"
           << "  \"benchmark\": \"chunk_render_readiness\",\n"
           << "  \"measurement_endpoint\": ";
    write_json_string(output, measurement_endpoint);
    output << ",\n"
           << "  \"excludes_gpu_execution_and_scanout\": true,\n"
           << "  \"config\": {\n"
           << "    \"render_backend\": ";
    write_json_string(output, rhi::render_backend_name(config.render_backend));
    output
        << ",\n"
        << "    \"seed\": " << config.seed << ",\n"
        << "    \"horizontal_radius_chunks\": " << config.horizontal_radius_chunks << ",\n"
        << "    \"warmup_repetitions\": " << config.warmup_repetitions << ",\n"
        << "    \"repetitions\": " << config.repetitions << ",\n"
        << "    \"update_interval_us\": " << config.update_interval_us << ",\n"
        << "    \"timeout_ms\": " << config.timeout_ms << ",\n"
        << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n"
        << "    \"maximum_draw_eligibility_p95_ms\": " << config.maximum_draw_eligibility_p95_ms
        << ",\n"
        << "    \"maximum_upload_preparation_ms\": " << config.maximum_upload_preparation_ms
        << ",\n"
        << "    \"maximum_synchronous_gpu_wait_ms\": " << config.maximum_synchronous_gpu_wait_ms
        << ",\n"
        << "    \"maximum_mesh_builds_per_publication\": "
        << config.maximum_mesh_builds_per_publication << ",\n"
        << "    \"loading\": {\"worker_count\": " << config.loading.worker_count
        << ", \"max_concurrent_requests\": " << config.loading.max_concurrent_requests
        << ", \"max_completed_results\": " << config.loading.max_completed_results
        << ", \"reservation_bytes_per_request\": " << config.loading.reservation_bytes_per_request
        << ", \"max_reserved_working_bytes\": " << config.loading.max_reserved_working_bytes
        << ", \"max_publications_per_update\": " << config.loading.max_publications_per_update
        << ", \"max_publication_time_us\": " << config.loading.max_publication_time_us << "},\n"
        << "    \"rendering\": {\"mesh_worker_count\": " << config.rendering.mesh_worker_count
        << ", \"max_concurrent_mesh_jobs\": " << config.rendering.max_concurrent_mesh_jobs
        << ", \"max_chunks_meshed_per_frame\": " << config.rendering.max_chunks_meshed_per_frame
        << ", \"max_chunks_uploaded_per_frame\": " << config.rendering.max_chunks_uploaded_per_frame
        << ", \"max_bytes_uploaded_per_frame\": " << config.rendering.max_bytes_uploaded_per_frame
        << ", \"max_snapshot_cells_per_frame\": " << config.rendering.max_snapshot_cells_per_frame
        << ", \"max_completed_mesh_results_per_frame\": "
        << config.rendering.max_completed_mesh_results_per_frame
        << ", \"gpu_terrain_budget_bytes\": " << config.rendering.gpu_terrain_budget_bytes << "}\n"
        << "  },\n";
    write_runtime_metadata(output, runtime);
    output << "  \"device\": {\"backend\": ";
    write_json_string(output, device.backend);
    output << ", \"device_name\": ";
    write_json_string(output, device.device_name);
    output << ", \"driver_name\": ";
    write_json_string(output, device.driver_name);
    output << ", \"driver_info\": ";
    write_json_string(output, device.driver_info);
    output << ", \"vendor_id\": " << device.vendor_id << ", \"device_id\": " << device.device_id
           << ", \"api_version\": " << device.api_version
           << ", \"driver_version\": " << device.driver_version
           << ", \"headless\": " << (device.headless ? "true" : "false") << "},\n";

    output << "  \"runs\": [\n";
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& run = runs[index];
        output << "    {\"repetition\": " << run.repetition
               << ", \"desired_chunks\": " << run.desired_chunks
               << ", \"owner_updates\": " << run.owner_updates
               << ", \"elapsed_us\": " << run.elapsed_us
               << ", \"submitted_requests\": " << run.submitted_requests
               << ", \"published_requests\": " << run.published_requests
               << ", \"draw_eligible_chunks\": " << run.draw_eligible_chunks
               << ", \"cancelled_requests\": " << run.cancelled_requests
               << ", \"stale_requests\": " << run.stale_requests
               << ", \"failed_requests\": " << run.failed_requests
               << ", \"rejected_requests\": " << run.rejected_requests
               << ", \"admission_deferred_updates\": " << run.admission_deferred_updates
               << ", \"load_item_budget_exhaustions\": " << run.load_item_budget_exhaustions
               << ", \"load_time_budget_exhaustions\": " << run.load_time_budget_exhaustions
               << ", \"maximum_publication_time_us\": " << run.maximum_publication_time_us
               << ", \"reserved_working_bytes_high_water\": "
               << run.reserved_working_bytes_high_water
               << ", \"final_reserved_working_bytes\": " << run.final_reserved_working_bytes
               << ", \"scheduled_mesh_jobs\": " << run.scheduled_mesh_jobs
               << ", \"completed_mesh_jobs\": " << run.completed_mesh_jobs
               << ", \"built_meshes\": " << run.built_meshes
               << ", \"published_meshes\": " << run.published_meshes
               << ", \"stale_mesh_results\": " << run.stale_mesh_results
               << ", \"cancelled_mesh_results\": " << run.cancelled_mesh_results
               << ", \"failed_mesh_results\": " << run.failed_mesh_results
               << ", \"failed_uploads\": " << run.failed_uploads
               << ", \"coalesced_mesh_invalidations\": " << run.coalesced_mesh_invalidations
               << ", \"abandoned_mesh_invalidations\": " << run.abandoned_mesh_invalidations
               << ", \"uploaded_chunks\": " << run.uploaded_chunks
               << ", \"uploaded_bytes\": " << run.uploaded_bytes
               << ", \"mesh_builds_per_publication\": " << run.mesh_builds_per_publication
               << ", \"pending_meshes\": " << run.pending_meshes
               << ", \"in_flight_meshes\": " << run.in_flight_meshes
               << ", \"pending_uploads\": " << run.pending_uploads
               << ", \"pending_edit_responses\": " << run.pending_edit_responses
               << ", \"current_mesh_stages\": " << run.current_mesh_stages
               << ", \"exact_resident_meshes\": " << run.exact_resident_meshes
               << ", \"drawable_resident_meshes\": " << run.drawable_resident_meshes
               << ", \"peak_gpu_resident_bytes\": " << run.peak_gpu_resident_bytes
               << ", \"maximum_snapshot_cells_per_update\": "
               << run.maximum_snapshot_cells_per_update
               << ", \"maximum_uploaded_bytes_per_update\": "
               << run.maximum_uploaded_bytes_per_update
               << ", \"maximum_owner_update_ms\": " << run.maximum_owner_update_ms
               << ", \"maximum_render_synchronize_ms\": " << run.maximum_render_synchronize_ms
               << ", \"maximum_draw_list_build_ms\": " << run.maximum_draw_list_build_ms
               << ", \"maximum_chunk_snapshot_ms\": " << run.maximum_chunk_snapshot_ms
               << ", \"maximum_meshing_ms\": " << run.maximum_meshing_ms
               << ", \"maximum_upload_preparation_ms\": " << run.maximum_upload_preparation_ms
               << ", \"maximum_upload_ms\": " << run.maximum_upload_ms
               << ", \"maximum_gpu_wait_ms\": " << run.maximum_gpu_wait_ms
               << ", \"baseline_live_render_resources\": " << run.baseline_live_render_resources
               << ", \"final_live_render_resources\": " << run.final_live_render_resources << '}'
               << (index + 1U == runs.size() ? "\n" : ",\n");
    }
    output << "  ],\n";

    output << "  \"raw_samples\": [\n";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output << "    {\"repetition\": " << sample.repetition
               << ", \"ordinal\": " << sample.ordinal << ", \"coord\": [" << sample.coord.x << ", "
               << sample.coord.y << ", " << sample.coord.z
               << "], \"request_id\": " << sample.request_id
               << ", \"load_generation\": " << sample.load_generation
               << ", \"mesh_request_revision\": " << sample.mesh_request_revision
               << ", \"source\": ";
        write_json_string(output, world::chunk_stream_load_source_name(sample.source));
        output << ", \"saved_edit_count\": " << sample.saved_edit_count
               << ", \"interest_to_publication_us\": " << sample.interest_to_publication_us
               << ", \"publication_to_mesh_resident_us\": "
               << sample.publication_to_mesh_resident_us
               << ", \"interest_to_mesh_resident_us\": " << sample.interest_to_mesh_resident_us
               << ", \"mesh_resident_to_draw_eligibility_us\": "
               << sample.mesh_resident_to_draw_eligibility_us
               << ", \"interest_to_draw_eligibility_us\": "
               << sample.interest_to_draw_eligibility_us
               << ", \"scheduler_pipeline_ms\": " << sample.scheduler_pipeline_ms
               << ", \"first_draw_owner_update\": " << sample.first_draw_owner_update
               << ", \"draw_command_count\": " << sample.draw_command_count
               << ", \"vertex_count\": " << sample.vertex_count
               << ", \"index_count\": " << sample.index_count
               << ", \"resident_bytes\": " << sample.resident_bytes << '}'
               << (index + 1U == raw_samples.size() ? "\n" : ",\n");
    }
    output << "  ],\n";

    const auto result = summary();
    output
        << "  \"summary\": {\n"
        << "    \"run_count\": " << result.run_count << ",\n"
        << "    \"sample_count\": " << result.sample_count << ",\n"
        << "    \"median_interest_to_publication_ms\": " << result.median_interest_to_publication_ms
        << ",\n"
        << "    \"p95_interest_to_publication_ms\": " << result.p95_interest_to_publication_ms
        << ",\n"
        << "    \"p99_interest_to_publication_ms\": " << result.p99_interest_to_publication_ms
        << ",\n"
        << "    \"median_publication_to_mesh_resident_ms\": "
        << result.median_publication_to_mesh_resident_ms << ",\n"
        << "    \"p95_publication_to_mesh_resident_ms\": "
        << result.p95_publication_to_mesh_resident_ms << ",\n"
        << "    \"p99_publication_to_mesh_resident_ms\": "
        << result.p99_publication_to_mesh_resident_ms << ",\n"
        << "    \"median_interest_to_mesh_resident_ms\": "
        << result.median_interest_to_mesh_resident_ms << ",\n"
        << "    \"p95_interest_to_mesh_resident_ms\": " << result.p95_interest_to_mesh_resident_ms
        << ",\n"
        << "    \"p99_interest_to_mesh_resident_ms\": " << result.p99_interest_to_mesh_resident_ms
        << ",\n"
        << "    \"median_interest_to_draw_eligibility_ms\": "
        << result.median_interest_to_draw_eligibility_ms << ",\n"
        << "    \"p95_interest_to_draw_eligibility_ms\": "
        << result.p95_interest_to_draw_eligibility_ms << ",\n"
        << "    \"p99_interest_to_draw_eligibility_ms\": "
        << result.p99_interest_to_draw_eligibility_ms << ",\n"
        << "    \"maximum_interest_to_draw_eligibility_ms\": "
        << result.maximum_interest_to_draw_eligibility_ms << ",\n"
        << "    \"mean_chunks_per_second\": " << result.mean_chunks_per_second << ",\n"
        << "    \"maximum_publication_time_us\": " << result.maximum_publication_time_us << ",\n"
        << "    \"reserved_working_bytes_high_water\": " << result.reserved_working_bytes_high_water
        << ",\n"
        << "    \"peak_gpu_resident_bytes\": " << result.peak_gpu_resident_bytes << ",\n"
        << "    \"maximum_snapshot_cells_per_update\": " << result.maximum_snapshot_cells_per_update
        << ",\n"
        << "    \"maximum_uploaded_bytes_per_update\": " << result.maximum_uploaded_bytes_per_update
        << ",\n"
        << "    \"maximum_mesh_builds_per_publication\": "
        << result.maximum_mesh_builds_per_publication << ",\n"
        << "    \"maximum_owner_update_ms\": " << result.maximum_owner_update_ms << ",\n"
        << "    \"maximum_render_synchronize_ms\": " << result.maximum_render_synchronize_ms
        << ",\n"
        << "    \"maximum_draw_list_build_ms\": " << result.maximum_draw_list_build_ms << ",\n"
        << "    \"maximum_chunk_snapshot_ms\": " << result.maximum_chunk_snapshot_ms << ",\n"
        << "    \"maximum_meshing_ms\": " << result.maximum_meshing_ms << ",\n"
        << "    \"maximum_upload_preparation_ms\": " << result.maximum_upload_preparation_ms
        << ",\n"
        << "    \"maximum_upload_ms\": " << result.maximum_upload_ms << ",\n"
        << "    \"maximum_gpu_wait_ms\": " << result.maximum_gpu_wait_ms << ",\n"
        << "    \"total_stale_mesh_results\": " << result.total_stale_mesh_results << ",\n"
        << "    \"total_cancelled_mesh_results\": " << result.total_cancelled_mesh_results << ",\n"
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

core::Status
ChunkRenderReadinessBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    return status ? write_text_file(path, to_json()) : status;
}

core::Result<ChunkRenderReadinessBenchmarkReport>
run_chunk_render_readiness_benchmark(const ChunkRenderReadinessBenchmarkConfig& config) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.chunk_render_readiness.run");
    auto status = config.validate();
    if (!status) {
        return core::Result<ChunkRenderReadinessBenchmarkReport>::failure(status.error().code,
                                                                          status.error().message);
    }

    for (std::uint32_t warmup = 0; warmup < config.warmup_repetitions; ++warmup) {
        auto execution = run_workload(config, warmup);
        if (!execution) {
            return core::Result<ChunkRenderReadinessBenchmarkReport>::failure(
                execution.error().code, execution.error().message);
        }
    }

    ChunkRenderReadinessBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();
    for (std::uint32_t repetition = 0; repetition < config.repetitions; ++repetition) {
        auto execution = run_workload(config, repetition);
        if (!execution) {
            return core::Result<ChunkRenderReadinessBenchmarkReport>::failure(
                execution.error().code, execution.error().message);
        }
        if (repetition == 0) {
            report.device = execution.value().device;
        } else if (execution.value().device.backend != report.device.backend ||
                   execution.value().device.device_name != report.device.device_name ||
                   execution.value().device.driver_name != report.device.driver_name ||
                   execution.value().device.driver_info != report.device.driver_info ||
                   execution.value().device.vendor_id != report.device.vendor_id ||
                   execution.value().device.device_id != report.device.device_id ||
                   execution.value().device.api_version != report.device.api_version ||
                   execution.value().device.driver_version != report.device.driver_version ||
                   execution.value().device.headless != report.device.headless) {
            return core::Result<ChunkRenderReadinessBenchmarkReport>::failure(
                "chunk_render_readiness_benchmark.device_changed",
                "render device provenance changed between retained repetitions");
        }
        report.runs.push_back(std::move(execution.value().run));
        for (auto& sample : execution.value().samples) {
            report.raw_samples.push_back(std::move(sample));
        }
    }
    status = report.validate();
    if (!status) {
        return core::Result<ChunkRenderReadinessBenchmarkReport>::failure(status.error().code,
                                                                          status.error().message);
    }
    return core::Result<ChunkRenderReadinessBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::renderer::benchmark
