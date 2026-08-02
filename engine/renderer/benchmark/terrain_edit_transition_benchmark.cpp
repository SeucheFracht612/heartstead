#include "engine/renderer/benchmark/terrain_edit_transition_benchmark.hpp"

#include "engine/profiling/profiler.hpp"
#include "engine/renderer/terrain/far_terrain_world_surface.hpp"
#include "engine/renderer/terrain/terrain_mapping.hpp"
#include "engine/world/voxels/voxel_palette.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <numeric>
#include <ranges>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace heartstead::renderer::benchmark {

namespace {

using BenchmarkClock = dirty::DirtyRegionClock;

constexpr world::ChunkCoord benchmark_center{};
constexpr world::VoxelCoord benchmark_edit_voxel{16, 1, 8};
constexpr world::VoxelCoord benchmark_supersession_voxel{16, 2, 8};
constexpr rhi::RenderResourceHandle benchmark_near_pipeline{41};
constexpr rhi::RenderResourceHandle benchmark_far_pipeline{42};

struct BenchmarkState {
    world::VoxelPalette palette;
    world::WorldState world;
    RenderCamera camera;
    math::Vec3d camera_world{};
    world::ChunkCoord edit_chunk;
    std::unique_ptr<rhi::IRenderDevice> device;
    std::unique_ptr<ChunkGpuCache> near_cache;
    std::unique_ptr<ChunkRenderSystem> near_renderer;
    std::unique_ptr<FarTerrainRenderer> far_renderer;
    FarTerrainWorldSurfaceCache surface_cache;
    std::size_t baseline_live_resources = 0;
};

struct OwnerUpdateObservation {
    ChunkDrawList near_draws;
    std::vector<rhi::RenderDrawCommand> far_draws;
    std::size_t invalidated_regions = 0;
    double elapsed_ms = 0.0;
};

struct WorkloadExecution {
    TerrainEditTransitionDeviceMetadata device;
    TerrainEditTransitionBenchmarkRun run;
    TerrainEditTransitionBenchmarkSample sample;
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

[[nodiscard]] std::size_t configured_chunk_count(std::uint16_t radius) noexcept {
    const auto side = static_cast<std::size_t>(radius) * 2U + 1U;
    return side * side;
}

[[nodiscard]] std::vector<world::ChunkCoord> benchmark_coordinates(std::uint16_t radius) {
    std::vector<world::ChunkCoord> result;
    result.reserve(configured_chunk_count(radius));
    const auto signed_radius = static_cast<std::int64_t>(radius);
    for (std::int64_t z = -signed_radius; z <= signed_radius; ++z) {
        for (std::int64_t x = -signed_radius; x <= signed_radius; ++x) {
            result.push_back({benchmark_center.x + x, benchmark_center.y, benchmark_center.z + z});
        }
    }
    std::ranges::sort(result, [](world::ChunkCoord left, world::ChunkCoord right) {
        const auto left_distance = left.x * left.x + left.z * left.z;
        const auto right_distance = right.x * right.x + right.z * right.z;
        return left_distance != right_distance ? left_distance < right_distance : left < right;
    });
    return result;
}

[[nodiscard]] core::Result<world::VoxelPalette> make_palette() {
    const auto prototype = core::PrototypeId::parse("benchmark:voxels/terrain_transition_stone");
    if (!prototype) {
        return core::Result<world::VoxelPalette>::failure(
            "terrain_edit_transition_benchmark.invalid_voxel_id",
            "the built-in terrain-transition voxel id is invalid");
    }
    world::VoxelDefinition stone;
    stone.type = 1;
    stone.prototype_id = *prototype;
    stone.display_name = "Terrain-transition benchmark stone";
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

[[nodiscard]] std::vector<world::VoxelCell> flat_chunk_cells() {
    std::vector<world::VoxelCell> cells(world::VoxelChunk::total_cells, world::VoxelCell::air());
    constexpr auto edge = static_cast<std::size_t>(world::VoxelChunk::edge_length);
    for (std::size_t z = 0; z < edge; ++z) {
        for (std::size_t x = 0; x < edge; ++x) {
            cells[z * edge * edge + x] = world::VoxelCell{1, 0};
        }
    }
    return cells;
}

[[nodiscard]] core::Result<RenderCamera> make_camera() {
    const auto origin = world::chunk_local_to_block(benchmark_center, {16, 16, 16});
    if (!origin) {
        return core::Result<RenderCamera>::failure(origin.error().code, origin.error().message);
    }
    RenderCamera camera;
    camera.floating_origin.block = origin.value();
    camera.local_position = {0.0F, 24.0F, 96.0F};
    camera.pitch_radians = -0.65F;
    camera.aspect_ratio = 2.0F;
    camera.far_plane = 512.0F;
    auto status = camera.update_matrices();
    if (!status) {
        return core::Result<RenderCamera>::failure(status.error().code, status.error().message);
    }
    return core::Result<RenderCamera>::success(camera);
}

[[nodiscard]] bool intersects_horizontally(const math::Bounds3d& left,
                                           const math::Bounds3d& right) noexcept {
    return left.min.x < right.max.x && left.max.x > right.min.x && left.min.z < right.max.z &&
           left.max.z > right.min.z;
}

[[nodiscard]] core::Result<world::ChunkCoord>
select_edit_chunk(const TerrainEditTransitionBenchmarkConfig& config, math::Vec3d camera_world) {
    auto clipmap = FarTerrainClipmap::create(config.far_rendering.clipmap);
    if (!clipmap) {
        return core::Result<world::ChunkCoord>::failure(clipmap.error().code,
                                                        clipmap.error().message);
    }
    const auto plan = clipmap.value().plan(camera_world);
    const world::BlockCoord camera_block{static_cast<std::int64_t>(std::floor(camera_world.x)),
                                         static_cast<std::int64_t>(std::floor(camera_world.y)),
                                         static_cast<std::int64_t>(std::floor(camera_world.z))};
    const auto camera_chunk = world::chunk_coord_for_block(camera_block);
    std::vector<world::ChunkCoord> candidates;
    std::ostringstream inspected;
    for (const auto coord : benchmark_coordinates(config.world_radius_chunks)) {
        const auto dx = std::abs(coord.x - camera_chunk.x);
        const auto dz = std::abs(coord.z - camera_chunk.z);
        const auto horizontal_radius =
            static_cast<std::int64_t>(config.near_rendering.distances.visible_horizontal_radius);
        if (coord.y != camera_chunk.y - 1 || dx > horizontal_radius || dz > horizontal_radius ||
            dx * dx + dz * dz > horizontal_radius * horizontal_radius ||
            coord.z >= camera_chunk.z) {
            continue;
        }
        const auto minimum = world::chunk_local_to_block(coord, {0, 0, 0});
        const auto maximum = world::chunk_local_to_block(
            coord, {static_cast<std::uint16_t>(world::VoxelChunk::edge_length - 1U), 0,
                    static_cast<std::uint16_t>(world::VoxelChunk::edge_length - 1U)});
        if (!minimum || !maximum) {
            continue;
        }
        const math::Bounds3d bounds{
            {static_cast<double>(minimum.value().x), 0.0, static_cast<double>(minimum.value().z)},
            {static_cast<double>(maximum.value().x) + 1.0, 0.0,
             static_cast<double>(maximum.value().z) + 1.0}};
        bool intersects_mid = false;
        bool intersects_far = false;
        for (const auto& patch : plan.patches) {
            if (!intersects_horizontally(bounds, patch.horizontal_bounds)) {
                continue;
            }
            if (patch.key.level < config.far_rendering.lod_updates.mid_level_count) {
                intersects_mid = true;
            } else {
                intersects_far = true;
            }
        }
        if (intersects_mid && intersects_far) {
            candidates.push_back(coord);
        }
        inspected << " [" << coord.x << ',' << coord.y << ',' << coord.z
                  << " mid=" << (intersects_mid ? 1 : 0) << " far=" << (intersects_far ? 1 : 0)
                  << ']';
    }
    std::ranges::sort(candidates, [camera_chunk](world::ChunkCoord left, world::ChunkCoord right) {
        const auto distance = [camera_chunk](world::ChunkCoord coord) {
            const auto dx = coord.x - camera_chunk.x;
            const auto dz = coord.z - camera_chunk.z;
            return dx * dx + dz * dz;
        };
        const auto left_distance = distance(left);
        const auto right_distance = distance(right);
        return left_distance != right_distance ? left_distance < right_distance : left < right;
    });
    if (candidates.empty()) {
        return core::Result<world::ChunkCoord>::failure(
            "terrain_edit_transition_benchmark.no_lod_boundary_chunk",
            "the configured corpus, camera, and clipmap expose no visible chunk that intersects "
            "both mid and far terrain patches; camera_chunk=[" +
                std::to_string(camera_chunk.x) + ',' + std::to_string(camera_chunk.y) + ',' +
                std::to_string(camera_chunk.z) + "] inspected:" + inspected.str());
    }
    return core::Result<world::ChunkCoord>::success(candidates.front());
}

[[nodiscard]] TerrainEditTransitionDeviceMetadata
device_metadata(const rhi::IRenderDevice& device) {
    const auto info = device.info();
    const auto capabilities = device.capabilities();
    TerrainEditTransitionDeviceMetadata result;
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
make_state(const TerrainEditTransitionBenchmarkConfig& config) {
    auto state = std::make_unique<BenchmarkState>();
    auto palette = make_palette();
    if (!palette) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(palette.error().code,
                                                                      palette.error().message);
    }
    state->palette = std::move(palette).value();
    auto camera = make_camera();
    if (!camera) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(camera.error().code,
                                                                      camera.error().message);
    }
    state->camera = camera.value();
    state->camera_world = {
        static_cast<double>(state->camera.floating_origin.block.x) + state->camera.local_position.x,
        static_cast<double>(state->camera.floating_origin.block.y) + state->camera.local_position.y,
        static_cast<double>(state->camera.floating_origin.block.z) + state->camera.local_position.z,
    };
    auto edit_chunk = select_edit_chunk(config, state->camera_world);
    if (!edit_chunk) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(edit_chunk.error().code,
                                                                      edit_chunk.error().message);
    }
    state->edit_chunk = edit_chunk.value();

    const auto cells = flat_chunk_cells();
    for (const auto coordinate : benchmark_coordinates(config.world_radius_chunks)) {
        world::VoxelChunk chunk(coordinate);
        auto status = chunk.load_generated_cells(cells);
        if (!status) {
            return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                          status.error().message);
        }
        status =
            state->world.chunks().insert_generated(std::move(chunk), state->world.dirty_regions());
        if (!status) {
            return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                          status.error().message);
        }
    }

    rhi::RenderDeviceDesc device_desc;
    device_desc.backend = config.render_backend;
    device_desc.application_name = "Heartstead Terrain Edit Transition Benchmark";
    device_desc.initial_extent = {1280, 640};
    device_desc.enable_validation = false;
    auto device = rhi::create_render_device(device_desc);
    if (!device) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(device.error().code,
                                                                      device.error().message);
    }
    state->device = std::move(device).value();
    state->baseline_live_resources = state->device->live_resource_count();

    rhi::RenderPipelineLayoutDesc far_layout;
    far_layout.material_id = *core::PrototypeId::parse("base:materials/far_terrain");
    far_layout.shader_template = {"base", "shaders/far_terrain.vert"};
    far_layout.descriptors = {
        {"far_patch_draws", rhi::RenderDescriptorKind::storage_buffer, 15, true,
         rhi::RenderShaderStageFlags::vertex},
    };
    auto layout_result = state->device->bind_pipeline_layout(std::move(far_layout));
    if (!layout_result) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(
            layout_result.error().code, layout_result.error().message);
    }

    state->near_cache = std::make_unique<ChunkGpuCache>(*state->device);
    auto status = state->near_cache->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                      status.error().message);
    }
    state->near_renderer = std::make_unique<ChunkRenderSystem>(
        *state->near_cache, benchmark_near_pipeline, &state->palette, config.near_rendering);
    status = state->near_renderer->initialize();
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                      status.error().message);
    }
    state->far_renderer = std::make_unique<FarTerrainRenderer>(*state->device);
    status = state->far_renderer->initialize(config.far_rendering, benchmark_far_pipeline);
    if (!status) {
        return core::Result<std::unique_ptr<BenchmarkState>>::failure(status.error().code,
                                                                      status.error().message);
    }
    return core::Result<std::unique_ptr<BenchmarkState>>::success(std::move(state));
}

[[nodiscard]] bool exact_near_mesh_is_resident(const BenchmarkState& state) {
    const auto* chunk = state.world.chunks().find(state.edit_chunk);
    if (chunk == nullptr) {
        return false;
    }
    const auto* entry = state.near_cache->find(chunk->identity());
    if (entry == nullptr || entry->state != ChunkGpuState::resident ||
        entry->resident_content_revision != chunk->content_revision() ||
        entry->resident_render_table_revision != state.palette.render_revision() ||
        !chunk->stages().record(world::ChunkStage::mesh).resident_is_current()) {
        return false;
    }
    return world::dependency_revisions_match(state.world.chunks(),
                                             entry->resident_dependency_revisions);
}

[[nodiscard]] std::size_t edit_draw_count(const BenchmarkState& state,
                                          const ChunkDrawList& draws) noexcept {
    const auto seed = terrain::terrain_coordinate_key(state.edit_chunk);
    return static_cast<std::size_t>(
        std::ranges::count(draws.draws, seed, &rhi::RenderDrawCommand::texture_variation_seed));
}

[[nodiscard]] bool near_is_idle(const ChunkRenderStats& stats) noexcept {
    return stats.pending_mesh_count == 0 && stats.in_flight_mesh_count == 0 &&
           stats.pending_upload_count == 0 && stats.pending_edit_to_visible_count == 0;
}

[[nodiscard]] bool far_is_idle(const FarTerrainRendererStats& stats) noexcept {
    return stats.pending_patches == 0 && stats.stale_resident_patches == 0 &&
           stats.in_flight_updates == 0 && stats.ready_meshes == 0 &&
           stats.worker_in_flight_meshes == 0 && stats.worker_completed_mailbox == 0;
}

[[nodiscard]] core::Result<OwnerUpdateObservation> update_renderers(BenchmarkState& state) {
    const auto started_at = BenchmarkClock::now();
    auto status = state.near_renderer->synchronize(state.world, state.camera);
    if (!status) {
        return core::Result<OwnerUpdateObservation>::failure(status.error().code,
                                                             status.error().message);
    }

    const auto surface_revision = far_terrain_world_surface_revision(state.world);
    std::vector<math::Bounds3d> invalidated_regions;
    if (surface_revision != state.surface_cache.revision()) {
        invalidated_regions = state.surface_cache.synchronize(state.world, surface_revision);
    }
    const FarTerrainSurfaceSampler sampler = [&state](double x, double z, FarTerrainDomain domain) {
        return state.surface_cache.sample(x, z, domain);
    };
    status = state.far_renderer->update(state.camera_world, sampler, surface_revision,
                                        invalidated_regions);
    if (!status) {
        return core::Result<OwnerUpdateObservation>::failure(status.error().code,
                                                             status.error().message);
    }

    OwnerUpdateObservation result;
    result.invalidated_regions = invalidated_regions.size();
    result.near_draws = state.near_renderer->build_draw_list(state.camera);
    result.far_draws = state.far_renderer->build_draws(state.camera);
    result.elapsed_ms = elapsed_milliseconds(started_at, BenchmarkClock::now());
    static_cast<void>(state.world.dirty_regions().consume_all());
    return core::Result<OwnerUpdateObservation>::success(std::move(result));
}

void pace_owner_thread(BenchmarkClock::time_point& next_update, BenchmarkClock::time_point deadline,
                       std::uint64_t interval_us) {
    next_update += std::chrono::microseconds(interval_us);
    std::this_thread::sleep_until(std::min(next_update, deadline));
}

[[nodiscard]] core::Status settle_initial_state(BenchmarkState& state,
                                                const TerrainEditTransitionBenchmarkConfig& config,
                                                TerrainEditTransitionBenchmarkRun& run) {
    const auto started_at = BenchmarkClock::now();
    const auto deadline = started_at + std::chrono::milliseconds(config.timeout_ms);
    auto next_update = started_at;
    while (true) {
        auto observation = update_renderers(state);
        if (!observation) {
            return core::Status::failure(observation.error().code, observation.error().message);
        }
        ++run.initial_owner_updates;
        const auto near_draws = edit_draw_count(state, observation.value().near_draws);
        const auto& near = state.near_renderer->stats();
        const auto& far = state.far_renderer->stats();
        if (near.total_failed_mesh_count != 0 || near.total_failed_upload_count != 0 ||
            far.total_mesh_jobs_failed != 0) {
            return core::Status::failure(
                "terrain_edit_transition_benchmark.initial_failure",
                "near or far terrain failed while establishing initial residency");
        }
        if (near_is_idle(near) && far_is_idle(far) && exact_near_mesh_is_resident(state) &&
            near_draws > 0 && !observation.value().far_draws.empty() &&
            far.resident_patches == far.planned_patches && far.resident_patches > 0) {
            run.loaded_chunks = state.world.chunks().chunk_count();
            run.initial_near_resident_chunks = near.cache.resident_chunk_count;
            run.initial_far_resident_patches = far.resident_patches;
            run.initial_near_draw_commands = near_draws;
            run.initial_far_draw_commands = observation.value().far_draws.size();
            run.initial_settlement_us = elapsed_microseconds(started_at, BenchmarkClock::now());
            state.near_renderer->reset_session_stats();
            state.near_cache->reset_session_stats();
            return core::Status::ok();
        }
        if (BenchmarkClock::now() >= deadline) {
            std::ostringstream detail;
            detail << "near_pending=" << near.pending_mesh_count
                   << ", near_flight=" << near.in_flight_mesh_count
                   << ", near_upload=" << near.pending_upload_count
                   << ", near_edit=" << near.pending_edit_to_visible_count
                   << ", near_exact=" << (exact_near_mesh_is_resident(state) ? 1 : 0)
                   << ", near_draws=" << near_draws << ", far_pending=" << far.pending_patches
                   << ", far_flight=" << far.worker_in_flight_meshes
                   << ", far_ready=" << far.ready_meshes
                   << ", far_mailbox=" << far.worker_completed_mailbox
                   << ", far_resident=" << far.resident_patches
                   << ", far_planned=" << far.planned_patches
                   << ", far_draws=" << observation.value().far_draws.size();
            return core::Status::failure(
                "terrain_edit_transition_benchmark.initial_timeout",
                "near and far terrain did not reach complete initial residency before timeout: " +
                    detail.str());
        }
        pace_owner_thread(next_update, deadline, config.update_interval_us);
    }
}

void record_update_high_water(TerrainEditTransitionBenchmarkSample& sample,
                              const OwnerUpdateObservation& observation,
                              const ChunkRenderStats& near, const FarTerrainRendererStats& far) {
    sample.maximum_owner_update_ms =
        std::max(sample.maximum_owner_update_ms, observation.elapsed_ms);
    sample.maximum_near_snapshot_ms =
        std::max(sample.maximum_near_snapshot_ms, near.chunk_snapshot_ms);
    sample.maximum_near_meshing_ms = std::max(sample.maximum_near_meshing_ms, near.meshing_ms);
    sample.maximum_near_upload_preparation_ms =
        std::max(sample.maximum_near_upload_preparation_ms, near.upload_preparation_ms);
    sample.maximum_near_upload_ms = std::max(sample.maximum_near_upload_ms, near.upload_ms);
    sample.maximum_synchronous_gpu_wait_ms =
        std::max(sample.maximum_synchronous_gpu_wait_ms, near.gpu_wait_ms);
    sample.maximum_far_worker_meshing_ms =
        std::max(sample.maximum_far_worker_meshing_ms, far.worker_meshing_ms);
    sample.maximum_near_pending_meshes =
        std::max(sample.maximum_near_pending_meshes, near.pending_mesh_count);
    sample.maximum_near_in_flight_meshes =
        std::max(sample.maximum_near_in_flight_meshes, near.in_flight_mesh_count);
    sample.maximum_near_pending_uploads =
        std::max(sample.maximum_near_pending_uploads, near.pending_upload_count);
    sample.maximum_far_ready_meshes = std::max(sample.maximum_far_ready_meshes, far.ready_meshes);
    sample.maximum_far_in_flight_meshes =
        std::max(sample.maximum_far_in_flight_meshes, far.worker_in_flight_meshes);
    sample.maximum_far_completed_mailbox =
        std::max(sample.maximum_far_completed_mailbox, far.worker_completed_mailbox);
    sample.maximum_far_pipeline_occupancy = std::max(
        sample.maximum_far_pipeline_occupancy, far.ready_meshes + far.worker_in_flight_meshes);
    sample.maximum_near_uploaded_bytes_per_update =
        std::max(sample.maximum_near_uploaded_bytes_per_update, near.uploaded_bytes);
    sample.maximum_far_uploaded_bytes_per_update =
        std::max(sample.maximum_far_uploaded_bytes_per_update, far.uploaded_bytes);
}

[[nodiscard]] core::Result<TerrainEditTransitionBenchmarkSample>
execute_isolated_edit(BenchmarkState& state, const TerrainEditTransitionBenchmarkConfig& config,
                      TerrainEditTransitionBenchmarkRun& run) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.terrain_edit_transition.isolated_edit");
    const auto far_before = state.far_renderer->stats();
    const auto started_at = BenchmarkClock::now();
    auto status =
        state.world.chunks().set(state.edit_chunk, benchmark_edit_voxel, world::VoxelCell{1, 0},
                                 state.world.dirty_regions(), state.palette);
    if (!status) {
        return core::Result<TerrainEditTransitionBenchmarkSample>::failure(status.error().code,
                                                                           status.error().message);
    }
    const auto* target_chunk = state.world.chunks().find(state.edit_chunk);
    if (target_chunk == nullptr) {
        return core::Result<TerrainEditTransitionBenchmarkSample>::failure(
            "terrain_edit_transition_benchmark.missing_edit_chunk",
            "the benchmark edit removed its authoritative center chunk");
    }

    TerrainEditTransitionBenchmarkSample sample;
    sample.repetition = run.repetition;
    sample.coord = state.edit_chunk;
    sample.voxel = benchmark_edit_voxel;
    sample.target_content_revision = target_chunk->content_revision();
    sample.target_surface_revision = far_terrain_world_surface_revision(state.world);
    sample.minimum_near_resident_chunks = run.initial_near_resident_chunks;
    sample.minimum_far_resident_patches = run.initial_far_resident_patches;
    sample.minimum_near_draw_commands = run.initial_near_draw_commands;
    sample.minimum_far_draw_commands = run.initial_far_draw_commands;

    const auto deadline = started_at + std::chrono::milliseconds(config.timeout_ms);
    auto next_update = started_at;
    while (true) {
        auto observation = update_renderers(state);
        if (!observation) {
            return core::Result<TerrainEditTransitionBenchmarkSample>::failure(
                observation.error().code, observation.error().message);
        }
        ++sample.owner_updates;
        const auto& near = state.near_renderer->stats();
        const auto& far = state.far_renderer->stats();
        const auto near_draws = edit_draw_count(state, observation.value().near_draws);
        sample.minimum_near_draw_commands = std::min(sample.minimum_near_draw_commands, near_draws);
        sample.minimum_far_draw_commands =
            std::min(sample.minimum_far_draw_commands, observation.value().far_draws.size());
        sample.minimum_near_resident_chunks =
            std::min(sample.minimum_near_resident_chunks, near.cache.resident_chunk_count);
        sample.minimum_far_resident_patches =
            std::min(sample.minimum_far_resident_patches, far.resident_patches);
        record_update_high_water(sample, observation.value(), near, far);
        sample.near_cancelled_mesh_results += near.cancelled_mesh_count;

        if (near_draws == 0 || observation.value().far_draws.empty() ||
            near.cache.resident_chunk_count < run.initial_near_resident_chunks ||
            far.resident_patches < run.initial_far_resident_patches) {
            return core::Result<TerrainEditTransitionBenchmarkSample>::failure(
                "terrain_edit_transition_benchmark.residency_hole",
                "an isolated edit removed retained near or far draw coverage before replacement");
        }
        if (near.total_failed_mesh_count != 0 || near.total_failed_upload_count != 0 ||
            far.total_mesh_jobs_failed != far_before.total_mesh_jobs_failed) {
            return core::Result<TerrainEditTransitionBenchmarkSample>::failure(
                "terrain_edit_transition_benchmark.edit_failure",
                "near or far terrain failed while processing an isolated edit");
        }

        sample.rebuilt_mid_patches += far.rebuilt_mid_patches;
        sample.rebuilt_far_patches += far.rebuilt_far_patches;
        const auto observed_at = BenchmarkClock::now();
        if (sample.near_draw_current_us == 0 && exact_near_mesh_is_resident(state) &&
            near_draws > 0) {
            sample.near_draw_current_us = elapsed_microseconds(started_at, observed_at);
        }
        if (sample.first_mid_publication_us == 0 && far.rebuilt_mid_patches > 0) {
            sample.first_mid_publication_us = elapsed_microseconds(started_at, observed_at);
        }
        if (sample.first_far_publication_us == 0 && far.rebuilt_far_patches > 0) {
            sample.first_far_publication_us = elapsed_microseconds(started_at, observed_at);
        }
        if (sample.mid_convergence_us == 0 && sample.first_mid_publication_us > 0 &&
            far.pending_mid_updates == 0) {
            sample.mid_convergence_us = elapsed_microseconds(started_at, observed_at);
        }
        if (sample.far_convergence_us == 0 && sample.first_far_publication_us > 0 &&
            far.pending_far_updates == 0) {
            sample.far_convergence_us = elapsed_microseconds(started_at, observed_at);
        }

        const auto near_complete = sample.near_draw_current_us > 0 && near_is_idle(near) &&
                                   near.total_edit_to_visible_completed_count == 1;
        const auto far_complete = sample.mid_convergence_us > 0 && sample.far_convergence_us > 0 &&
                                  far_is_idle(far) &&
                                  state.surface_cache.revision() == sample.target_surface_revision;
        if (near_complete && far_complete) {
            sample.full_convergence_us = elapsed_microseconds(started_at, observed_at);
            sample.instrumented_near_edit_to_visible_ms = near.edit_to_visible_latest_ms;
            sample.near_completed_mesh_jobs = near.total_completed_mesh_job_count;
            sample.near_built_meshes = near.total_built_mesh_count;
            sample.near_published_meshes = near.total_published_mesh_count;
            sample.near_stale_mesh_results = near.total_stale_mesh_result_count;
            sample.invalidated_far_patches =
                far.total_invalidated_patches - far_before.total_invalidated_patches;
            sample.far_submitted_mesh_jobs =
                far.total_mesh_jobs_submitted - far_before.total_mesh_jobs_submitted;
            sample.far_completed_mesh_jobs =
                far.total_mesh_jobs_completed - far_before.total_mesh_jobs_completed;
            sample.far_cancelled_mesh_jobs =
                far.total_mesh_jobs_cancelled - far_before.total_mesh_jobs_cancelled;
            sample.far_stale_results = far.total_stale_results - far_before.total_stale_results;
            return core::Result<TerrainEditTransitionBenchmarkSample>::success(std::move(sample));
        }
        if (BenchmarkClock::now() >= deadline) {
            std::ostringstream detail;
            detail << "near_us=" << sample.near_draw_current_us
                   << ", near_pending=" << near.pending_mesh_count
                   << ", near_flight=" << near.in_flight_mesh_count
                   << ", near_upload=" << near.pending_upload_count
                   << ", near_edit=" << near.pending_edit_to_visible_count
                   << ", near_complete=" << near.total_edit_to_visible_completed_count
                   << ", mid_first=" << sample.first_mid_publication_us
                   << ", mid_done=" << sample.mid_convergence_us
                   << ", mid_pending=" << far.pending_mid_updates
                   << ", far_first=" << sample.first_far_publication_us
                   << ", far_done=" << sample.far_convergence_us
                   << ", far_pending=" << far.pending_far_updates
                   << ", far_stale_resident=" << far.stale_resident_patches << ", invalidated="
                   << far.total_invalidated_patches - far_before.total_invalidated_patches
                   << ", rebuilt_mid=" << sample.rebuilt_mid_patches
                   << ", rebuilt_far=" << sample.rebuilt_far_patches;
            return core::Result<TerrainEditTransitionBenchmarkSample>::failure(
                "terrain_edit_transition_benchmark.edit_timeout",
                "an isolated edit did not converge across near, mid, and far terrain: " +
                    detail.str());
        }
        pace_owner_thread(next_update, deadline, config.update_interval_us);
    }
}

[[nodiscard]] core::Status
execute_supersession_probe(BenchmarkState& state,
                           const TerrainEditTransitionBenchmarkConfig& config,
                           TerrainEditTransitionBenchmarkRun& run) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.terrain_edit_transition.supersession");
    state.near_renderer->reset_session_stats();
    state.near_cache->reset_session_stats();
    const auto far_before = state.far_renderer->stats();
    run.supersession_minimum_near_draw_commands = run.initial_near_draw_commands;
    run.supersession_minimum_far_draw_commands = run.initial_far_draw_commands;
    run.supersession_minimum_near_resident_chunks = run.initial_near_resident_chunks;
    run.supersession_minimum_far_resident_patches = run.initial_far_resident_patches;

    const auto started_at = BenchmarkClock::now();
    auto status = state.world.chunks().set(state.edit_chunk, benchmark_supersession_voxel,
                                           world::VoxelCell{1, 0}, state.world.dirty_regions(),
                                           state.palette);
    if (!status) {
        return status;
    }
    auto first = update_renderers(state);
    if (!first) {
        return core::Status::failure(first.error().code, first.error().message);
    }
    ++run.supersession_owner_updates;
    run.supersession_minimum_near_draw_commands =
        std::min(run.supersession_minimum_near_draw_commands,
                 edit_draw_count(state, first.value().near_draws));
    run.supersession_minimum_far_draw_commands =
        std::min(run.supersession_minimum_far_draw_commands, first.value().far_draws.size());
    run.supersession_minimum_near_resident_chunks =
        std::min(run.supersession_minimum_near_resident_chunks,
                 state.near_renderer->stats().cache.resident_chunk_count);
    run.supersession_minimum_far_resident_patches =
        std::min(run.supersession_minimum_far_resident_patches,
                 state.far_renderer->stats().resident_patches);

    status = state.world.chunks().set(state.edit_chunk, benchmark_supersession_voxel,
                                      world::VoxelCell::air(), state.world.dirty_regions(),
                                      state.palette);
    if (!status) {
        return status;
    }
    const auto target_surface_revision = far_terrain_world_surface_revision(state.world);
    const auto deadline = started_at + std::chrono::milliseconds(config.timeout_ms);
    auto next_update = started_at;
    std::uint64_t near_cancelled_results = 0;
    while (true) {
        auto observation = update_renderers(state);
        if (!observation) {
            return core::Status::failure(observation.error().code, observation.error().message);
        }
        ++run.supersession_owner_updates;
        const auto& near = state.near_renderer->stats();
        const auto& far = state.far_renderer->stats();
        near_cancelled_results += near.cancelled_mesh_count;
        const auto near_draws = edit_draw_count(state, observation.value().near_draws);
        run.supersession_minimum_near_draw_commands =
            std::min(run.supersession_minimum_near_draw_commands, near_draws);
        run.supersession_minimum_far_draw_commands = std::min(
            run.supersession_minimum_far_draw_commands, observation.value().far_draws.size());
        run.supersession_minimum_near_resident_chunks = std::min(
            run.supersession_minimum_near_resident_chunks, near.cache.resident_chunk_count);
        run.supersession_minimum_far_resident_patches =
            std::min(run.supersession_minimum_far_resident_patches, far.resident_patches);

        if (near_draws == 0 || observation.value().far_draws.empty() ||
            near.cache.resident_chunk_count < run.initial_near_resident_chunks ||
            far.resident_patches < run.initial_far_resident_patches) {
            return core::Status::failure("terrain_edit_transition_benchmark.supersession_hole",
                                         "superseded terrain work removed retained draw coverage");
        }
        if (near.total_failed_mesh_count != 0 || near.total_failed_upload_count != 0 ||
            far.total_mesh_jobs_failed != far_before.total_mesh_jobs_failed) {
            return core::Status::failure(
                "terrain_edit_transition_benchmark.supersession_failure",
                "near or far terrain failed during the supersession probe");
        }

        if (near_is_idle(near) && far_is_idle(far) && exact_near_mesh_is_resident(state) &&
            state.surface_cache.revision() == target_surface_revision) {
            run.supersession_convergence_us =
                elapsed_microseconds(started_at, BenchmarkClock::now());
            run.supersession_near_coalesced_invalidations =
                near.total_coalesced_edit_invalidation_count;
            run.supersession_near_obsolete_results =
                near.total_stale_mesh_result_count + near_cancelled_results;
            run.supersession_far_coalesced_invalidations =
                far.total_coalesced_invalidations - far_before.total_coalesced_invalidations;
            run.supersession_far_stale_results =
                far.total_stale_results - far_before.total_stale_results;
            if (run.supersession_near_coalesced_invalidations == 0 ||
                run.supersession_far_coalesced_invalidations == 0 ||
                run.supersession_far_stale_results == 0) {
                return core::Status::failure(
                    "terrain_edit_transition_benchmark.missing_supersession_evidence",
                    "the forced edit race converged without observable stale-work rejection");
            }
            return core::Status::ok();
        }
        if (BenchmarkClock::now() >= deadline) {
            return core::Status::failure(
                "terrain_edit_transition_benchmark.supersession_timeout",
                "the final authoritative edit did not converge after superseding active work");
        }
        pace_owner_thread(next_update, deadline, config.update_interval_us);
    }
}

[[nodiscard]] core::Status shutdown_state(BenchmarkState& state,
                                          TerrainEditTransitionBenchmarkRun& run) {
    auto first_failure = state.far_renderer->shutdown();
    state.near_renderer->shutdown();
    auto status = state.device->wait_idle();
    if (!status && first_failure) {
        first_failure = status;
    }
    status = state.near_cache->shutdown();
    if (!status && first_failure) {
        first_failure = status;
    }
    run.baseline_live_render_resources = state.baseline_live_resources;
    run.final_live_render_resources = state.device->live_resource_count();
    if (run.final_live_render_resources != run.baseline_live_render_resources && first_failure) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.render_resource_leak",
            "the terrain-transition workload did not release every render resource it created");
    }
    return first_failure;
}

[[nodiscard]] core::Result<WorkloadExecution>
run_workload(const TerrainEditTransitionBenchmarkConfig& config, std::uint32_t repetition) {
    HEARTSTEAD_PROFILE_ZONE_NAMED("benchmark.terrain_edit_transition.workload");
    auto created = make_state(config);
    if (!created) {
        return core::Result<WorkloadExecution>::failure(created.error().code,
                                                        created.error().message);
    }
    auto state = std::move(created).value();
    WorkloadExecution execution;
    execution.device = device_metadata(*state->device);
    execution.run.repetition = repetition;
    auto status = settle_initial_state(*state, config, execution.run);
    if (!status) {
        return core::Result<WorkloadExecution>::failure(status.error().code,
                                                        status.error().message);
    }
    auto sample = execute_isolated_edit(*state, config, execution.run);
    if (!sample) {
        return core::Result<WorkloadExecution>::failure(sample.error().code,
                                                        sample.error().message);
    }
    execution.sample = std::move(sample).value();
    status = execute_supersession_probe(*state, config, execution.run);
    if (!status) {
        return core::Result<WorkloadExecution>::failure(status.error().code,
                                                        status.error().message);
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
            return core::Status::failure(
                "terrain_edit_transition_benchmark.create_directory_failed",
                "failed to create benchmark output directory: " + error.message());
        }
    }
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return core::Status::failure("terrain_edit_transition_benchmark.open_output_failed",
                                     "failed to open benchmark output: " + path.string());
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!stream) {
        return core::Status::failure("terrain_edit_transition_benchmark.write_output_failed",
                                     "failed to write benchmark output: " + path.string());
    }
    return core::Status::ok();
}

} // namespace

ChunkRenderConfig terrain_edit_transition_near_defaults() noexcept {
    ChunkRenderConfig config;
    config.distances.simulation_radius = 1;
    config.distances.loaded_horizontal_radius = 4;
    config.distances.loaded_vertical_radius = 2;
    config.distances.mesh_horizontal_radius = 2;
    config.distances.mesh_vertical_radius = 1;
    config.distances.gpu_resident_horizontal_radius = 2;
    config.distances.gpu_resident_vertical_radius = 1;
    config.distances.visible_horizontal_radius = 2;
    config.distances.visible_vertical_radius = 1;
    config.distances.gpu_resident_hysteresis = 1;
    return config;
}

FarTerrainRendererConfig terrain_edit_transition_far_defaults() noexcept {
    FarTerrainRendererConfig config;
    config.clipmap.level_count = 3;
    config.clipmap.patches_per_axis = 5;
    config.clipmap.patch_resolution = 8;
    config.clipmap.base_cell_size = 2.0;
    config.clipmap.maximum_distance = 512.0;
    config.clipmap.inner_exclusion_radius = 16.0;
    config.lod_updates.mid_level_count = 2;
    config.lod_updates.maximum_mid_rebuilds_per_frame = 2;
    config.lod_updates.maximum_far_rebuilds_per_frame = 1;
    config.mesh_scheduler.worker_count = 2;
    config.mesh_scheduler.maximum_concurrent_jobs = 3;
    config.maximum_patch_builds_per_frame = 3;
    config.maximum_upload_bytes_per_frame = 2U * 1024U * 1024U;
    config.maximum_resident_bytes = 32U * 1024U * 1024U;
    config.maximum_replacement_headroom_bytes = 8U * 1024U * 1024U;
    return config;
}

core::Status TerrainEditTransitionBenchmarkConfig::validate() const {
    if (render_backend != rhi::RenderBackend::headless &&
        render_backend != rhi::RenderBackend::vulkan) {
        return core::Status::failure("terrain_edit_transition_benchmark.invalid_backend",
                                     "render backend must be headless or Vulkan");
    }
    if (world_radius_chunks < 2 || world_radius_chunks > 8 ||
        world_radius_chunks < near_rendering.distances.visible_horizontal_radius) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.invalid_world_radius",
            "world radius must be 2..8 chunks and cover the near visible radius");
    }
    if (repetitions == 0 || repetitions > 100 || warmup_repetitions > 100) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.invalid_repetitions",
            "terrain-transition repetitions must be 1..100 and warmups 0..100");
    }
    if (update_interval_us == 0 || update_interval_us > 1'000'000 || timeout_ms == 0 ||
        timeout_ms > 600'000) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.invalid_timing",
            "owner cadence and workload timeout must be positive and bounded");
    }
    const std::array positive_gates{
        maximum_near_draw_p95_ms,       maximum_mid_convergence_p95_ms,
        maximum_far_convergence_p95_ms, maximum_full_convergence_p95_ms,
        maximum_owner_update_ms,        maximum_upload_preparation_ms,
    };
    if (std::ranges::any_of(positive_gates,
                            [](double value) { return !std::isfinite(value) || value <= 0.0; }) ||
        !std::isfinite(maximum_synchronous_gpu_wait_ms) || maximum_synchronous_gpu_wait_ms < 0.0) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.invalid_gates",
            "terrain-transition latency and owner-work gates must be finite and bounded");
    }
    auto status = near_rendering.validate();
    if (!status) {
        return status;
    }
    if (far_rendering.lod_updates.mid_level_count == 0 ||
        far_rendering.lod_updates.mid_level_count >= far_rendering.clipmap.level_count ||
        far_rendering.maximum_patch_builds_per_frame == 0 ||
        far_rendering.maximum_upload_bytes_per_frame == 0 ||
        far_rendering.maximum_resident_bytes == 0 ||
        far_rendering.maximum_replacement_headroom_bytes == 0 ||
        far_rendering.maximum_replacement_headroom_bytes >
            std::numeric_limits<std::size_t>::max() - far_rendering.maximum_resident_bytes) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.invalid_far_config",
            "far terrain must contain nonempty mid/far bands and positive bounded budgets");
    }
    auto clipmap = FarTerrainClipmap::create(far_rendering.clipmap);
    if (!clipmap) {
        return core::Status::failure(clipmap.error().code, clipmap.error().message);
    }
    auto graph = FarTerrainLodUpdateGraph::create(far_rendering.lod_updates,
                                                  far_rendering.clipmap.level_count,
                                                  far_rendering.maximum_patch_builds_per_frame);
    if (!graph) {
        return core::Status::failure(graph.error().code, graph.error().message);
    }
    status = far_rendering.mesh_scheduler.validate();
    if (!status) {
        return status;
    }
    auto camera = make_camera();
    if (!camera) {
        return core::Status::failure(camera.error().code, camera.error().message);
    }
    const math::Vec3d camera_world{
        static_cast<double>(camera.value().floating_origin.block.x) +
            camera.value().local_position.x,
        static_cast<double>(camera.value().floating_origin.block.y) +
            camera.value().local_position.y,
        static_cast<double>(camera.value().floating_origin.block.z) +
            camera.value().local_position.z,
    };
    auto edit_chunk = select_edit_chunk(*this, camera_world);
    return edit_chunk ? core::Status::ok()
                      : core::Status::failure(edit_chunk.error().code, edit_chunk.error().message);
}

core::Status TerrainEditTransitionBenchmarkReport::validate() const {
    auto status = config.validate();
    if (!status) {
        return status;
    }
    if (runs.size() != config.repetitions || raw_samples.size() != config.repetitions) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.incomplete_report",
            "terrain-transition report does not retain one run and sample per repetition");
    }
    if (device.backend != rhi::render_backend_name(config.render_backend) ||
        device.device_name.empty() || device.driver_name.empty()) {
        return core::Status::failure(
            "terrain_edit_transition_benchmark.invalid_device_metadata",
            "terrain-transition report has incomplete or inconsistent device provenance");
    }
    const auto camera = make_camera().value();
    const math::Vec3d camera_world{
        static_cast<double>(camera.floating_origin.block.x) + camera.local_position.x,
        static_cast<double>(camera.floating_origin.block.y) + camera.local_position.y,
        static_cast<double>(camera.floating_origin.block.z) + camera.local_position.z,
    };
    const auto expected_edit_chunk = select_edit_chunk(config, camera_world).value();

    std::set<std::uint32_t> run_keys;
    for (const auto& run : runs) {
        if (run.repetition >= config.repetitions || !run_keys.insert(run.repetition).second ||
            run.loaded_chunks != configured_chunk_count(config.world_radius_chunks) ||
            run.initial_near_resident_chunks == 0 || run.initial_far_resident_patches == 0 ||
            run.initial_near_draw_commands == 0 || run.initial_far_draw_commands == 0 ||
            run.initial_owner_updates == 0 || run.initial_settlement_us == 0 ||
            run.supersession_owner_updates < 2 || run.supersession_convergence_us == 0 ||
            run.supersession_near_coalesced_invalidations == 0 ||
            run.supersession_far_coalesced_invalidations == 0 ||
            run.supersession_far_stale_results == 0 ||
            run.supersession_minimum_near_draw_commands == 0 ||
            run.supersession_minimum_far_draw_commands == 0 ||
            run.supersession_minimum_near_resident_chunks < run.initial_near_resident_chunks ||
            run.supersession_minimum_far_resident_patches < run.initial_far_resident_patches ||
            run.baseline_live_render_resources != run.final_live_render_resources) {
            return core::Status::failure("terrain_edit_transition_benchmark.invalid_run",
                                         "a terrain-transition run failed settlement, "
                                         "supersession, continuity, or teardown");
        }
    }

    std::set<std::uint32_t> sample_keys;
    for (const auto& sample : raw_samples) {
        const auto durations_are_ordered =
            sample.near_draw_current_us > 0 && sample.first_mid_publication_us > 0 &&
            sample.mid_convergence_us >= sample.first_mid_publication_us &&
            sample.first_far_publication_us > 0 &&
            sample.far_convergence_us >= sample.first_far_publication_us &&
            sample.full_convergence_us >= sample.near_draw_current_us &&
            sample.full_convergence_us >= sample.mid_convergence_us &&
            sample.full_convergence_us >= sample.far_convergence_us;
        if (sample.repetition >= config.repetitions || !run_keys.contains(sample.repetition) ||
            !sample_keys.insert(sample.repetition).second || sample.coord != expected_edit_chunk ||
            sample.voxel != benchmark_edit_voxel || sample.target_content_revision == 0 ||
            sample.target_surface_revision == 0 || sample.owner_updates == 0 ||
            !durations_are_ordered || !std::isfinite(sample.instrumented_near_edit_to_visible_ms) ||
            sample.instrumented_near_edit_to_visible_ms <= 0.0 ||
            !std::isfinite(sample.maximum_owner_update_ms) ||
            sample.maximum_owner_update_ms < 0.0 || sample.minimum_near_resident_chunks == 0 ||
            sample.minimum_far_resident_patches == 0 || sample.minimum_near_draw_commands == 0 ||
            sample.minimum_far_draw_commands == 0 || sample.invalidated_far_patches == 0 ||
            sample.rebuilt_mid_patches == 0 || sample.rebuilt_far_patches == 0 ||
            sample.invalidated_far_patches !=
                sample.rebuilt_mid_patches + sample.rebuilt_far_patches ||
            sample.near_completed_mesh_jobs < sample.near_built_meshes ||
            sample.near_built_meshes < sample.near_published_meshes ||
            sample.near_published_meshes == 0 || sample.near_stale_mesh_results != 0 ||
            sample.near_cancelled_mesh_results != 0 ||
            sample.far_submitted_mesh_jobs < sample.invalidated_far_patches ||
            sample.far_completed_mesh_jobs < sample.invalidated_far_patches ||
            sample.far_cancelled_mesh_jobs != 0 || sample.far_stale_results != 0 ||
            sample.maximum_far_pipeline_occupancy >
                config.far_rendering.mesh_scheduler.maximum_concurrent_jobs ||
            sample.maximum_far_completed_mailbox >
                config.far_rendering.mesh_scheduler.maximum_completed_results ||
            sample.maximum_near_uploaded_bytes_per_update >
                config.near_rendering.max_bytes_uploaded_per_frame ||
            sample.maximum_far_uploaded_bytes_per_update >
                config.far_rendering.maximum_upload_bytes_per_frame) {
            return core::Status::failure(
                "terrain_edit_transition_benchmark.invalid_sample",
                "terrain-transition report contains invalid, censored, stale, or unbounded work");
        }
        const auto run = std::ranges::find(runs, sample.repetition,
                                           &TerrainEditTransitionBenchmarkRun::repetition);
        if (run == runs.end() ||
            sample.minimum_near_resident_chunks < run->initial_near_resident_chunks ||
            sample.minimum_far_resident_patches < run->initial_far_resident_patches) {
            return core::Status::failure(
                "terrain_edit_transition_benchmark.sample_residency_hole",
                "a retained edit sample dropped below its established resident baseline");
        }
    }
    return core::Status::ok();
}

TerrainEditTransitionBenchmarkSummary TerrainEditTransitionBenchmarkReport::summary() const {
    TerrainEditTransitionBenchmarkSummary result;
    result.run_count = runs.size();
    result.sample_count = raw_samples.size();
    std::vector<double> near_ms;
    std::vector<double> mid_ms;
    std::vector<double> far_ms;
    std::vector<double> full_ms;
    near_ms.reserve(raw_samples.size());
    mid_ms.reserve(raw_samples.size());
    far_ms.reserve(raw_samples.size());
    full_ms.reserve(raw_samples.size());
    for (const auto& sample : raw_samples) {
        near_ms.push_back(static_cast<double>(sample.near_draw_current_us) / 1'000.0);
        mid_ms.push_back(static_cast<double>(sample.mid_convergence_us) / 1'000.0);
        far_ms.push_back(static_cast<double>(sample.far_convergence_us) / 1'000.0);
        full_ms.push_back(static_cast<double>(sample.full_convergence_us) / 1'000.0);
        result.maximum_owner_update_ms =
            std::max(result.maximum_owner_update_ms, sample.maximum_owner_update_ms);
        result.maximum_upload_preparation_ms = std::max(result.maximum_upload_preparation_ms,
                                                        sample.maximum_near_upload_preparation_ms);
        result.maximum_synchronous_gpu_wait_ms = std::max(result.maximum_synchronous_gpu_wait_ms,
                                                          sample.maximum_synchronous_gpu_wait_ms);
        result.maximum_far_pipeline_occupancy =
            std::max(result.maximum_far_pipeline_occupancy, sample.maximum_far_pipeline_occupancy);
        result.total_invalidated_far_patches += sample.invalidated_far_patches;
        result.total_rebuilt_mid_patches += sample.rebuilt_mid_patches;
        result.total_rebuilt_far_patches += sample.rebuilt_far_patches;
        result.total_near_stale_or_cancelled_results +=
            sample.near_stale_mesh_results + sample.near_cancelled_mesh_results;
        result.total_far_stale_or_cancelled_results +=
            sample.far_stale_results + sample.far_cancelled_mesh_jobs;
    }
    for (const auto& run : runs) {
        result.total_near_stale_or_cancelled_results += run.supersession_near_obsolete_results;
        result.total_far_stale_or_cancelled_results += run.supersession_far_stale_results;
    }
    std::ranges::sort(near_ms);
    std::ranges::sort(mid_ms);
    std::ranges::sort(far_ms);
    std::ranges::sort(full_ms);
    result.median_near_draw_ms = percentile(near_ms, 0.50);
    result.p95_near_draw_ms = percentile(near_ms, 0.95);
    result.p99_near_draw_ms = percentile(near_ms, 0.99);
    result.median_mid_convergence_ms = percentile(mid_ms, 0.50);
    result.p95_mid_convergence_ms = percentile(mid_ms, 0.95);
    result.p99_mid_convergence_ms = percentile(mid_ms, 0.99);
    result.median_far_convergence_ms = percentile(far_ms, 0.50);
    result.p95_far_convergence_ms = percentile(far_ms, 0.95);
    result.p99_far_convergence_ms = percentile(far_ms, 0.99);
    result.median_full_convergence_ms = percentile(full_ms, 0.50);
    result.p95_full_convergence_ms = percentile(full_ms, 0.95);
    result.p99_full_convergence_ms = percentile(full_ms, 0.99);

    result.gates.evaluated = config.enforce_gates;
    if (config.enforce_gates) {
        const auto check = [&result](std::string metric, double actual, double limit) {
            if (!std::isfinite(actual) || actual > limit) {
                result.gates.violations.push_back({std::move(metric), actual, limit});
            }
        };
        check("p95_near_draw_ms", result.p95_near_draw_ms, config.maximum_near_draw_p95_ms);
        check("p95_mid_convergence_ms", result.p95_mid_convergence_ms,
              config.maximum_mid_convergence_p95_ms);
        check("p95_far_convergence_ms", result.p95_far_convergence_ms,
              config.maximum_far_convergence_p95_ms);
        check("p95_full_convergence_ms", result.p95_full_convergence_ms,
              config.maximum_full_convergence_p95_ms);
        check("maximum_owner_update_ms", result.maximum_owner_update_ms,
              config.maximum_owner_update_ms);
        check("maximum_upload_preparation_ms", result.maximum_upload_preparation_ms,
              config.maximum_upload_preparation_ms);
        check("maximum_synchronous_gpu_wait_ms", result.maximum_synchronous_gpu_wait_ms,
              config.maximum_synchronous_gpu_wait_ms);
        result.gates.passed = result.gates.violations.empty();
    }
    return result;
}

bool TerrainEditTransitionBenchmarkReport::gates_passed() const {
    return summary().gates.passed;
}

std::string TerrainEditTransitionBenchmarkReport::to_json() const {
    const auto totals = summary();
    std::ostringstream output;
    output << std::fixed << std::setprecision(6);
    output << "{\n"
           << "  \"schema_version\": " << schema_version << ",\n"
           << "  \"benchmark\": \"terrain_edit_transition\",\n"
           << "  \"measurement_endpoints\": {\n"
           << "    \"near\": \"" << near_measurement_endpoint << "\",\n"
           << "    \"mid\": \"" << mid_measurement_endpoint << "\",\n"
           << "    \"far\": \"" << far_measurement_endpoint << "\"\n"
           << "  },\n"
           << "  \"excludes_gpu_execution_presentation_and_scanout\": true,\n"
           << "  \"config\": {\n"
           << "    \"render_backend\": \"" << rhi::render_backend_name(config.render_backend)
           << "\",\n"
           << "    \"world_radius_chunks\": " << config.world_radius_chunks << ",\n"
           << "    \"warmup_repetitions\": " << config.warmup_repetitions << ",\n"
           << "    \"repetitions\": " << config.repetitions << ",\n"
           << "    \"update_interval_us\": " << config.update_interval_us << ",\n"
           << "    \"timeout_ms\": " << config.timeout_ms << ",\n"
           << "    \"enforce_gates\": " << (config.enforce_gates ? "true" : "false") << ",\n"
           << "    \"gates\": {\n"
           << "      \"maximum_near_draw_p95_ms\": " << config.maximum_near_draw_p95_ms << ",\n"
           << "      \"maximum_mid_convergence_p95_ms\": " << config.maximum_mid_convergence_p95_ms
           << ",\n"
           << "      \"maximum_far_convergence_p95_ms\": " << config.maximum_far_convergence_p95_ms
           << ",\n"
           << "      \"maximum_full_convergence_p95_ms\": "
           << config.maximum_full_convergence_p95_ms << ",\n"
           << "      \"maximum_owner_update_ms\": " << config.maximum_owner_update_ms << ",\n"
           << "      \"maximum_upload_preparation_ms\": " << config.maximum_upload_preparation_ms
           << ",\n"
           << "      \"maximum_synchronous_gpu_wait_ms\": "
           << config.maximum_synchronous_gpu_wait_ms << "\n"
           << "    },\n"
           << "    \"near_rendering\": {\n"
           << "      \"mesh_horizontal_radius\": "
           << config.near_rendering.distances.mesh_horizontal_radius << ",\n"
           << "      \"visible_horizontal_radius\": "
           << config.near_rendering.distances.visible_horizontal_radius << ",\n"
           << "      \"max_chunks_meshed_per_frame\": "
           << config.near_rendering.max_chunks_meshed_per_frame << ",\n"
           << "      \"max_chunks_uploaded_per_frame\": "
           << config.near_rendering.max_chunks_uploaded_per_frame << ",\n"
           << "      \"max_bytes_uploaded_per_frame\": "
           << config.near_rendering.max_bytes_uploaded_per_frame << ",\n"
           << "      \"max_snapshot_cells_per_frame\": "
           << config.near_rendering.max_snapshot_cells_per_frame << ",\n"
           << "      \"mesh_worker_count\": " << config.near_rendering.mesh_worker_count << ",\n"
           << "      \"max_concurrent_mesh_jobs\": "
           << config.near_rendering.max_concurrent_mesh_jobs << "\n"
           << "    },\n"
           << "    \"far_rendering\": {\n"
           << "      \"level_count\": " << config.far_rendering.clipmap.level_count << ",\n"
           << "      \"patches_per_axis\": " << config.far_rendering.clipmap.patches_per_axis
           << ",\n"
           << "      \"patch_resolution\": " << config.far_rendering.clipmap.patch_resolution
           << ",\n"
           << "      \"base_cell_size\": " << config.far_rendering.clipmap.base_cell_size << ",\n"
           << "      \"inner_exclusion_radius\": "
           << config.far_rendering.clipmap.inner_exclusion_radius << ",\n"
           << "      \"mid_level_count\": " << config.far_rendering.lod_updates.mid_level_count
           << ",\n"
           << "      \"maximum_mid_rebuilds_per_frame\": "
           << config.far_rendering.lod_updates.maximum_mid_rebuilds_per_frame << ",\n"
           << "      \"maximum_far_rebuilds_per_frame\": "
           << config.far_rendering.lod_updates.maximum_far_rebuilds_per_frame << ",\n"
           << "      \"maximum_patch_builds_per_frame\": "
           << config.far_rendering.maximum_patch_builds_per_frame << ",\n"
           << "      \"mesh_worker_count\": " << config.far_rendering.mesh_scheduler.worker_count
           << ",\n"
           << "      \"maximum_concurrent_mesh_jobs\": "
           << config.far_rendering.mesh_scheduler.maximum_concurrent_jobs << ",\n"
           << "      \"maximum_completed_results\": "
           << config.far_rendering.mesh_scheduler.maximum_completed_results << ",\n"
           << "      \"maximum_upload_bytes_per_frame\": "
           << config.far_rendering.maximum_upload_bytes_per_frame << ",\n"
           << "      \"maximum_resident_bytes\": " << config.far_rendering.maximum_resident_bytes
           << ",\n"
           << "      \"maximum_replacement_headroom_bytes\": "
           << config.far_rendering.maximum_replacement_headroom_bytes << "\n"
           << "    }\n"
           << "  },\n";
    write_runtime_metadata(output, runtime);
    output << "  \"device\": {\n"
           << "    \"backend\": ";
    write_json_string(output, device.backend);
    output << ",\n    \"device_name\": ";
    write_json_string(output, device.device_name);
    output << ",\n    \"driver_name\": ";
    write_json_string(output, device.driver_name);
    output << ",\n    \"driver_info\": ";
    write_json_string(output, device.driver_info);
    output << ",\n"
           << "    \"vendor_id\": " << device.vendor_id << ",\n"
           << "    \"device_id\": " << device.device_id << ",\n"
           << "    \"api_version\": " << device.api_version << ",\n"
           << "    \"driver_version\": " << device.driver_version << ",\n"
           << "    \"headless\": " << (device.headless ? "true" : "false") << "\n"
           << "  },\n"
           << "  \"summary\": {\n"
           << "    \"run_count\": " << totals.run_count << ",\n"
           << "    \"sample_count\": " << totals.sample_count << ",\n"
           << "    \"median_near_draw_ms\": " << totals.median_near_draw_ms << ",\n"
           << "    \"p95_near_draw_ms\": " << totals.p95_near_draw_ms << ",\n"
           << "    \"p99_near_draw_ms\": " << totals.p99_near_draw_ms << ",\n"
           << "    \"median_mid_convergence_ms\": " << totals.median_mid_convergence_ms << ",\n"
           << "    \"p95_mid_convergence_ms\": " << totals.p95_mid_convergence_ms << ",\n"
           << "    \"p99_mid_convergence_ms\": " << totals.p99_mid_convergence_ms << ",\n"
           << "    \"median_far_convergence_ms\": " << totals.median_far_convergence_ms << ",\n"
           << "    \"p95_far_convergence_ms\": " << totals.p95_far_convergence_ms << ",\n"
           << "    \"p99_far_convergence_ms\": " << totals.p99_far_convergence_ms << ",\n"
           << "    \"median_full_convergence_ms\": " << totals.median_full_convergence_ms << ",\n"
           << "    \"p95_full_convergence_ms\": " << totals.p95_full_convergence_ms << ",\n"
           << "    \"p99_full_convergence_ms\": " << totals.p99_full_convergence_ms << ",\n"
           << "    \"maximum_owner_update_ms\": " << totals.maximum_owner_update_ms << ",\n"
           << "    \"maximum_upload_preparation_ms\": " << totals.maximum_upload_preparation_ms
           << ",\n"
           << "    \"maximum_synchronous_gpu_wait_ms\": " << totals.maximum_synchronous_gpu_wait_ms
           << ",\n"
           << "    \"maximum_far_pipeline_occupancy\": " << totals.maximum_far_pipeline_occupancy
           << ",\n"
           << "    \"total_invalidated_far_patches\": " << totals.total_invalidated_far_patches
           << ",\n"
           << "    \"total_rebuilt_mid_patches\": " << totals.total_rebuilt_mid_patches << ",\n"
           << "    \"total_rebuilt_far_patches\": " << totals.total_rebuilt_far_patches << ",\n"
           << "    \"total_near_stale_or_cancelled_results\": "
           << totals.total_near_stale_or_cancelled_results << ",\n"
           << "    \"total_far_stale_or_cancelled_results\": "
           << totals.total_far_stale_or_cancelled_results << ",\n"
           << "    \"gates\": {\n"
           << "      \"evaluated\": " << (totals.gates.evaluated ? "true" : "false") << ",\n"
           << "      \"passed\": " << (totals.gates.passed ? "true" : "false") << ",\n"
           << "      \"violations\": [";
    for (std::size_t index = 0; index < totals.gates.violations.size(); ++index) {
        const auto& violation = totals.gates.violations[index];
        output << (index == 0 ? "\n" : ",\n") << "        {\"metric\": ";
        write_json_string(output, violation.metric);
        output << ", \"actual\": " << violation.actual << ", \"limit\": " << violation.limit << '}';
    }
    if (!totals.gates.violations.empty()) {
        output << '\n';
    }
    output << "      ]\n"
           << "    }\n"
           << "  },\n"
           << "  \"runs\": [";
    for (std::size_t index = 0; index < runs.size(); ++index) {
        const auto& run = runs[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"repetition\": " << run.repetition
               << ", \"loaded_chunks\": " << run.loaded_chunks
               << ", \"initial_near_resident_chunks\": " << run.initial_near_resident_chunks
               << ", \"initial_far_resident_patches\": " << run.initial_far_resident_patches
               << ", \"initial_near_draw_commands\": " << run.initial_near_draw_commands
               << ", \"initial_far_draw_commands\": " << run.initial_far_draw_commands
               << ", \"initial_owner_updates\": " << run.initial_owner_updates
               << ", \"initial_settlement_us\": " << run.initial_settlement_us
               << ", \"supersession_owner_updates\": " << run.supersession_owner_updates
               << ", \"supersession_convergence_us\": " << run.supersession_convergence_us
               << ", \"supersession_near_coalesced_invalidations\": "
               << run.supersession_near_coalesced_invalidations
               << ", \"supersession_near_obsolete_results\": "
               << run.supersession_near_obsolete_results
               << ", \"supersession_far_coalesced_invalidations\": "
               << run.supersession_far_coalesced_invalidations
               << ", \"supersession_far_stale_results\": " << run.supersession_far_stale_results
               << ", \"supersession_minimum_near_draw_commands\": "
               << run.supersession_minimum_near_draw_commands
               << ", \"supersession_minimum_far_draw_commands\": "
               << run.supersession_minimum_far_draw_commands
               << ", \"supersession_minimum_near_resident_chunks\": "
               << run.supersession_minimum_near_resident_chunks
               << ", \"supersession_minimum_far_resident_patches\": "
               << run.supersession_minimum_far_resident_patches
               << ", \"baseline_live_render_resources\": " << run.baseline_live_render_resources
               << ", \"final_live_render_resources\": " << run.final_live_render_resources << '}';
    }
    if (!runs.empty()) {
        output << '\n';
    }
    output << "  ],\n"
           << "  \"raw_samples\": [";
    for (std::size_t index = 0; index < raw_samples.size(); ++index) {
        const auto& sample = raw_samples[index];
        output << (index == 0 ? "\n" : ",\n") << "    {\"repetition\": " << sample.repetition
               << ", \"coord\": [" << sample.coord.x << ", " << sample.coord.y << ", "
               << sample.coord.z << "]" << ", \"voxel\": [" << sample.voxel.x << ", "
               << sample.voxel.y << ", " << sample.voxel.z << "]"
               << ", \"target_content_revision\": " << sample.target_content_revision
               << ", \"target_surface_revision\": " << sample.target_surface_revision
               << ", \"owner_updates\": " << sample.owner_updates
               << ", \"near_draw_current_us\": " << sample.near_draw_current_us
               << ", \"first_mid_publication_us\": " << sample.first_mid_publication_us
               << ", \"mid_convergence_us\": " << sample.mid_convergence_us
               << ", \"first_far_publication_us\": " << sample.first_far_publication_us
               << ", \"far_convergence_us\": " << sample.far_convergence_us
               << ", \"full_convergence_us\": " << sample.full_convergence_us
               << ", \"instrumented_near_edit_to_visible_ms\": "
               << sample.instrumented_near_edit_to_visible_ms
               << ", \"maximum_owner_update_ms\": " << sample.maximum_owner_update_ms
               << ", \"maximum_near_snapshot_ms\": " << sample.maximum_near_snapshot_ms
               << ", \"maximum_near_meshing_ms\": " << sample.maximum_near_meshing_ms
               << ", \"maximum_near_upload_preparation_ms\": "
               << sample.maximum_near_upload_preparation_ms
               << ", \"maximum_near_upload_ms\": " << sample.maximum_near_upload_ms
               << ", \"maximum_synchronous_gpu_wait_ms\": "
               << sample.maximum_synchronous_gpu_wait_ms
               << ", \"maximum_far_worker_meshing_ms\": " << sample.maximum_far_worker_meshing_ms
               << ", \"maximum_near_pending_meshes\": " << sample.maximum_near_pending_meshes
               << ", \"maximum_near_in_flight_meshes\": " << sample.maximum_near_in_flight_meshes
               << ", \"maximum_near_pending_uploads\": " << sample.maximum_near_pending_uploads
               << ", \"maximum_far_ready_meshes\": " << sample.maximum_far_ready_meshes
               << ", \"maximum_far_in_flight_meshes\": " << sample.maximum_far_in_flight_meshes
               << ", \"maximum_far_completed_mailbox\": " << sample.maximum_far_completed_mailbox
               << ", \"maximum_far_pipeline_occupancy\": " << sample.maximum_far_pipeline_occupancy
               << ", \"minimum_near_resident_chunks\": " << sample.minimum_near_resident_chunks
               << ", \"minimum_far_resident_patches\": " << sample.minimum_far_resident_patches
               << ", \"minimum_near_draw_commands\": " << sample.minimum_near_draw_commands
               << ", \"minimum_far_draw_commands\": " << sample.minimum_far_draw_commands
               << ", \"maximum_near_uploaded_bytes_per_update\": "
               << sample.maximum_near_uploaded_bytes_per_update
               << ", \"maximum_far_uploaded_bytes_per_update\": "
               << sample.maximum_far_uploaded_bytes_per_update
               << ", \"invalidated_far_patches\": " << sample.invalidated_far_patches
               << ", \"rebuilt_mid_patches\": " << sample.rebuilt_mid_patches
               << ", \"rebuilt_far_patches\": " << sample.rebuilt_far_patches
               << ", \"near_completed_mesh_jobs\": " << sample.near_completed_mesh_jobs
               << ", \"near_built_meshes\": " << sample.near_built_meshes
               << ", \"near_published_meshes\": " << sample.near_published_meshes
               << ", \"near_stale_mesh_results\": " << sample.near_stale_mesh_results
               << ", \"near_cancelled_mesh_results\": " << sample.near_cancelled_mesh_results
               << ", \"far_submitted_mesh_jobs\": " << sample.far_submitted_mesh_jobs
               << ", \"far_completed_mesh_jobs\": " << sample.far_completed_mesh_jobs
               << ", \"far_cancelled_mesh_jobs\": " << sample.far_cancelled_mesh_jobs
               << ", \"far_stale_results\": " << sample.far_stale_results << '}';
    }
    if (!raw_samples.empty()) {
        output << '\n';
    }
    output << "  ]\n}\n";
    return output.str();
}

core::Status
TerrainEditTransitionBenchmarkReport::write_json(const std::filesystem::path& path) const {
    auto status = validate();
    if (!status) {
        return status;
    }
    return write_text_file(path, to_json());
}

core::Result<TerrainEditTransitionBenchmarkReport>
run_terrain_edit_transition_benchmark(const TerrainEditTransitionBenchmarkConfig& config) {
    auto status = config.validate();
    if (!status) {
        return core::Result<TerrainEditTransitionBenchmarkReport>::failure(status.error().code,
                                                                           status.error().message);
    }
    TerrainEditTransitionBenchmarkReport report;
    report.config = config;
    report.runtime = profiling::query_runtime_metadata();

    for (std::uint32_t warmup = 0; warmup < config.warmup_repetitions; ++warmup) {
        auto execution = run_workload(config, warmup);
        if (!execution) {
            return core::Result<TerrainEditTransitionBenchmarkReport>::failure(
                execution.error().code, execution.error().message);
        }
        if (report.device.backend.empty()) {
            report.device = execution.value().device;
        }
    }
    report.runs.reserve(config.repetitions);
    report.raw_samples.reserve(config.repetitions);
    for (std::uint32_t repetition = 0; repetition < config.repetitions; ++repetition) {
        auto execution = run_workload(config, repetition);
        if (!execution) {
            return core::Result<TerrainEditTransitionBenchmarkReport>::failure(
                execution.error().code, execution.error().message);
        }
        if (report.device.backend.empty()) {
            report.device = execution.value().device;
        } else if (execution.value().device.backend != report.device.backend ||
                   execution.value().device.device_name != report.device.device_name ||
                   execution.value().device.driver_name != report.device.driver_name ||
                   execution.value().device.driver_version != report.device.driver_version) {
            return core::Result<TerrainEditTransitionBenchmarkReport>::failure(
                "terrain_edit_transition_benchmark.device_changed",
                "render device provenance changed between benchmark repetitions");
        }
        report.runs.push_back(std::move(execution.value().run));
        report.raw_samples.push_back(std::move(execution.value().sample));
    }
    status = report.validate();
    if (!status) {
        return core::Result<TerrainEditTransitionBenchmarkReport>::failure(status.error().code,
                                                                           status.error().message);
    }
    return core::Result<TerrainEditTransitionBenchmarkReport>::success(std::move(report));
}

} // namespace heartstead::renderer::benchmark
