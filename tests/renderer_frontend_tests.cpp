#include "engine/profiling/cpu_timing.hpp"
#include "engine/renderer/camera/frustum.hpp"
#include "engine/renderer/chunks/chunk_gpu_cache.hpp"
#include "engine/renderer/chunks/chunk_render_system.hpp"
#include "engine/renderer/render_camera.hpp"
#include "engine/renderer/renderer.hpp"
#include "engine/renderer/rhi/render_device.hpp"
#include "engine/world/world_state.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <ranges>
#include <thread>
#include <utility>

namespace {

void test_scoped_cpu_timing_zones() {
    heartstead::profiling::CpuTimingRecorder timings;
    {
        heartstead::profiling::ScopedCpuTimingZone zone(
            timings, heartstead::profiling::CpuTimingZone::meshing);
        std::uint64_t work = 0;
        for (std::uint64_t index = 0; index < 10'000; ++index) {
            work += index;
        }
        assert(work > 0);
    }
    assert(timings.milliseconds(heartstead::profiling::CpuTimingZone::meshing) > 0.0);
    assert(heartstead::profiling::cpu_timing_zone_name(
               heartstead::profiling::CpuTimingZone::complete_frame) == "complete_frame");
    assert(heartstead::profiling::cpu_timing_zone_name(
               heartstead::profiling::CpuTimingZone::visibility_culling) == "visibility_culling");
    timings.reset();
    assert(timings.milliseconds(heartstead::profiling::CpuTimingZone::meshing) == 0.0);
}

void test_frustum_aabb_culling() {
    heartstead::renderer::RenderCamera camera;
    camera.aspect_ratio = 1.0F;
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    assert(camera.update_matrices());

    const auto frustum =
        heartstead::renderer::RenderFrustum::from_view_projection(camera.view_projection);
    assert(frustum.is_valid());
    assert(frustum.intersects({{-0.5F, -0.5F, -5.0F}, {0.5F, 0.5F, -4.0F}}));
    assert(!frustum.intersects({{-0.5F, -0.5F, 4.0F}, {0.5F, 0.5F, 5.0F}}));
    assert(!frustum.intersects({{200.0F, -0.5F, -5.0F}, {201.0F, 0.5F, -4.0F}}));
    assert(!frustum.intersects({{-0.5F, -0.5F, -101.0F}, {0.5F, 0.5F, -100.1F}}));

    // Invalid bounds are deliberately conservative while renderer data is being debugged.
    assert(frustum.intersects({{1.0F, 1.0F, 1.0F}, {-1.0F, -1.0F, -1.0F}}));
}

void test_frame_builder_preserves_terrain_phase_commands() {
    using namespace heartstead;
    renderer::FrameBuilder builder({640, 360}, {0.1F, 0.2F, 0.3F, 1.0F});
    renderer::RenderCamera camera;
    assert(camera.set_aspect_ratio(640.0F / 360.0F));
    const renderer::rhi::RenderDrawCommand draw{{1}, {2}, {3}, 3};
    renderer::RenderCommandLists commands;
    commands.sky_draws.push_back(draw);
    commands.opaque_terrain_draws.push_back(draw);
    commands.alpha_tested_terrain_draws.push_back(draw);
    commands.transparent_terrain_draws.push_back(draw);
    auto frame = builder.build(camera, std::move(commands));
    assert(frame);
    assert(frame.value().plan.passes.size() == 8);
    assert(frame.value().plan.passes[0].name == "sky");
    assert(frame.value().plan.passes[1].name == "opaque_terrain");
    assert(frame.value().plan.passes[2].name == "alpha_tested_terrain");
    assert(frame.value().plan.passes[4].name == "transparent_terrain");
    assert(frame.value().plan.passes[7].name == "present");
    assert(frame.value().pass_commands.size() == 4);
    assert(frame.value().pass_commands[0].pass_index == 0);
    assert(frame.value().pass_commands[1].pass_index == 1);
    assert(frame.value().pass_commands[2].pass_index == 2);
    assert(frame.value().pass_commands[3].pass_index == 4);
}

void test_chunk_gpu_cache_lifecycle() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    device_desc.initial_extent = {640, 360};
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline_resources = device.value()->live_resource_count();

    renderer::ChunkGpuCache cache(*device.value());
    assert(cache.initialize());
    const world::ChunkIdentity identity{{3, -2, 7}, 41};
    assert(cache.insert(identity));
    assert(!cache.insert(identity));
    assert(!cache.insert({identity.coordinate, 42}));

    const std::array<renderer::terrain::GpuChunkVertex, 3> vertices{};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const math::Bounds3f bounds{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}};
    auto first_upload = cache.replace_mesh(identity, 5, bounds, vertices, indices);
    assert(first_upload);
    assert(first_upload.value().uploaded_bytes == sizeof(vertices) + 8U);
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    const auto* first_entry = cache.find(identity);
    assert(first_entry != nullptr);
    assert(first_entry->has_drawable_mesh());
    assert(first_entry->resident_content_revision == 5);
    assert(first_entry->mesh.index_type == renderer::rhi::RenderIndexType::uint16);
    assert(first_entry->mesh.sections.size() == 1);
    assert(first_entry->mesh.sections.front().first_index == 0);
    assert(first_entry->mesh.sections.front().index_count == indices.size());
    assert(cache.stats().uint16_index_chunk_count == 1);
    assert(cache.stats().uint32_index_chunk_count == 0);
    const auto first_vertex_allocation = first_entry->mesh.vertices;
    const auto first_index_allocation = first_entry->mesh.indices;

    auto rejected_replacement = cache.replace_mesh(identity, 6, bounds, vertices, {});
    assert(!rejected_replacement);
    assert(cache.find(identity)->mesh.vertices == first_vertex_allocation);
    assert(cache.find(identity)->mesh.indices == first_index_allocation);
    assert(cache.find(identity)->resident_content_revision == 5);

    const std::array<world::ChunkMeshSection, 1> invalid_sections{
        world::ChunkMeshSection{1, world::MeshingRenderPhase::opaque, 1, 3}};
    auto rejected_sections =
        cache.replace_mesh(identity, 6, bounds, vertices, indices, 1, {}, invalid_sections);
    assert(!rejected_sections);
    assert(rejected_sections.error().code == "chunk_gpu_cache.invalid_mesh_section");
    assert(cache.find(identity)->mesh.vertices == first_vertex_allocation);

    auto replacement = cache.replace_mesh(identity, 6, bounds, vertices, indices);
    assert(replacement);
    assert(replacement.value().replaced_resident_mesh);
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    const auto* replaced_entry = cache.find(identity);
    assert(replaced_entry != nullptr);
    assert(replaced_entry->mesh.vertices != first_vertex_allocation);
    assert(replaced_entry->mesh.indices != first_index_allocation);
    assert(replaced_entry->resident_content_revision == 6);

    const std::vector<renderer::terrain::GpuChunkVertex> pathological_vertices(65'537);
    const std::array<std::uint32_t, 3> pathological_indices{65'536, 1, 2};
    auto uint32_replacement =
        cache.replace_mesh(identity, 7, bounds, pathological_vertices, pathological_indices);
    assert(uint32_replacement);
    assert(cache.find(identity)->mesh.index_type == renderer::rhi::RenderIndexType::uint32);
    assert(cache.stats().uint16_index_chunk_count == 0);
    assert(cache.stats().uint32_index_chunk_count == 1);

    assert(cache.release_mesh(identity));
    assert(cache.contains(identity));
    assert(cache.find(identity)->state == renderer::ChunkGpuState::missing);
    assert(cache.find(identity)->last_resident_bytes > 0);
    assert(cache.stats().resident_chunk_count == 0);
    assert(cache.stats().vertex_arena.used_bytes == 0);
    assert(cache.stats().index_arena.used_bytes == 0);

    auto empty_replacement = cache.replace_mesh(identity, 8, {}, {}, {});
    assert(empty_replacement);
    assert(empty_replacement.value().empty);
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    assert(cache.find(identity)->state == renderer::ChunkGpuState::resident);
    assert(!cache.find(identity)->has_drawable_mesh());
    assert(cache.stats().resident_chunk_count == 1);
    assert(cache.stats().empty_chunk_count == 1);
    assert(cache.stats().uint16_index_chunk_count == 0);
    assert(cache.stats().uint32_index_chunk_count == 0);
    assert(cache.stats().resident_section_count == 0);
    assert(cache.stats().uploaded_chunk_count == 4);

    auto stale_erase = cache.erase({identity.coordinate, 40});
    assert(!stale_erase);
    assert(stale_erase.error().code == "chunk_gpu_cache.stale_load_generation");
    assert(cache.contains(identity));
    assert(cache.erase(identity));
    assert(cache.stats().entry_count == 0);
    assert(cache.stats().resident_allocation_count == 0);
    assert(cache.stats().vertex_arena.used_bytes == 0);
    assert(cache.stats().index_arena.used_bytes == 0);
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    assert(cache.shutdown());
    assert(device.value()->live_resource_count() == baseline_resources);
}

void test_chunk_draws_use_phase_pipelines_and_transparent_ordering() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    renderer::ChunkGpuCache cache(*device.value());
    assert(cache.initialize());

    constexpr renderer::TerrainPipelineSet pipelines{{101}, {102}, {103}, {104}};
    renderer::ChunkRenderConfig config;
    renderer::ChunkRenderSystem chunks(cache, pipelines, nullptr, config);
    assert(chunks.initialize());

    const world::ChunkIdentity identity{{0, 0, 0}, 1};
    assert(cache.insert(identity));
    const std::array<renderer::terrain::GpuChunkVertex, 4> vertices{};
    const std::array<std::uint32_t, 12> indices{0, 1, 2, 0, 2, 3, 0, 1, 2, 0, 2, 3};
    const std::array<world::ChunkMeshSection, 4> sections{
        world::ChunkMeshSection{1, world::MeshingRenderPhase::opaque, 0, 3},
        world::ChunkMeshSection{2, world::MeshingRenderPhase::alpha_tested, 3, 3},
        world::ChunkMeshSection{3, world::MeshingRenderPhase::transparent, 6, 3},
        world::ChunkMeshSection{4, world::MeshingRenderPhase::fluid, 9, 3},
    };
    assert(cache.replace_mesh(identity, 1, {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}, vertices,
                              indices, 1, {}, sections));

    renderer::RenderCamera camera;
    camera.floating_origin.block = {16, 16, 64};
    camera.aspect_ratio = 1.0F;
    camera.far_plane = 256.0F;
    assert(camera.update_matrices());
    const auto draws = chunks.build_draw_list(camera);
    assert(draws.draws.size() == 4);
    assert(draws.draws[0].pipeline == pipelines.opaque);
    assert(draws.draws[1].pipeline == pipelines.alpha_tested);
    assert(draws.draws[2].pipeline == pipelines.transparent);
    assert(draws.draws[3].pipeline == pipelines.fluid);
    assert(draws.draws[2].sort_depth > 0.0F);
    assert(draws.draws[3].sort_depth == draws.draws[2].sort_depth);

    chunks.shutdown();
    assert(cache.shutdown());
}

void test_chunk_render_distances_and_residency_hysteresis() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    renderer::ChunkGpuCache cache(*device.value());
    assert(cache.initialize());

    renderer::ChunkRenderConfig config;
    config.max_chunks_meshed_per_frame = 4;
    config.distances.simulation_radius = 1;
    config.distances.loaded_horizontal_radius = 4;
    config.distances.loaded_vertical_radius = 2;
    config.distances.mesh_horizontal_radius = 2;
    config.distances.mesh_vertical_radius = 1;
    config.distances.gpu_resident_horizontal_radius = 2;
    config.distances.gpu_resident_vertical_radius = 1;
    config.distances.visible_horizontal_radius = 1;
    config.distances.visible_vertical_radius = 1;
    config.distances.gpu_resident_hysteresis = 1;
    renderer::ChunkRenderSystem chunks(cache, {9002}, nullptr, config);

    constexpr std::int64_t large = 1'000'000'000;
    const world::ChunkCoord center{large, 0, -large};
    const world::ChunkCoord ahead{large, 0, -large - 1};
    const world::ChunkCoord resident_shell{large + 2, 0, -large};
    const world::ChunkCoord loaded_only{large + 4, 0, -large};
    world::WorldState world;
    assert(world.chunks().set(center, {1, 1, 1}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(ahead, {1, 1, 1}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(resident_shell, {1, 1, 1}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(loaded_only, {1, 1, 1}, world::VoxelCell{1, 255}));

    const auto center_identity = world.chunks().find(center)->identity();
    const auto loaded_only_identity = world.chunks().find(loaded_only)->identity();
    renderer::RenderCamera camera;
    auto camera_block = world::chunk_local_to_block(center, {16, 16, 16});
    assert(camera_block);
    camera.floating_origin.block = camera_block.value();
    camera.far_plane = 512.0F;
    assert(camera.update_matrices());

    const auto synchronize_until = [&](const auto& predicate) {
        for (std::size_t attempt = 0; attempt < 5'000; ++attempt) {
            assert(chunks.synchronize(world, camera));
            if (predicate()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(false && "distance-budgeted chunk renderer did not settle");
    };
    synchronize_until([&] {
        return cache.stats().resident_chunk_count == 3 &&
               cache.find(loaded_only_identity) != nullptr &&
               cache.find(loaded_only_identity)->state == renderer::ChunkGpuState::missing;
    });
    assert(cache.stats().entry_count == 4);
    auto draws = chunks.build_draw_list(camera);
    assert(draws.distance_culled_chunk_count >= 1);
    assert(draws.culled_chunk_count ==
           draws.distance_culled_chunk_count + draws.frustum_culled_chunk_count);

    camera_block = world::chunk_local_to_block(loaded_only, {16, 16, 16});
    assert(camera_block);
    camera.floating_origin.block = camera_block.value();
    assert(camera.update_matrices());
    synchronize_until([&] {
        return cache.find(loaded_only_identity)->state == renderer::ChunkGpuState::resident &&
               cache.find(center_identity)->state == renderer::ChunkGpuState::missing;
    });
    assert(cache.stats().entry_count == 4);
    assert(chunks.stats().distance_evicted_mesh_count > 0);

    camera_block = world::chunk_local_to_block(center, {16, 16, 16});
    assert(camera_block);
    camera.floating_origin.block = camera_block.value();
    assert(camera.update_matrices());
    synchronize_until([&] {
        return cache.find(center_identity)->state == renderer::ChunkGpuState::resident &&
               cache.find(loaded_only_identity)->state == renderer::ChunkGpuState::missing;
    });

    chunks.shutdown();
    assert(cache.shutdown());
}

void test_chunk_gpu_residency_budget_prefers_near_visible_meshes() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    renderer::ChunkGpuCache cache(*device.value());
    assert(cache.initialize());

    renderer::ChunkRenderConfig config;
    config.max_chunks_meshed_per_frame = 3;
    config.max_chunks_uploaded_per_frame = 3;
    config.gpu_terrain_budget_bytes = 700;
    config.distances.simulation_radius = 1;
    config.distances.loaded_horizontal_radius = 4;
    config.distances.loaded_vertical_radius = 1;
    config.distances.mesh_horizontal_radius = 4;
    config.distances.mesh_vertical_radius = 1;
    config.distances.gpu_resident_horizontal_radius = 4;
    config.distances.gpu_resident_vertical_radius = 1;
    config.distances.visible_horizontal_radius = 4;
    config.distances.visible_vertical_radius = 1;
    config.distances.gpu_resident_hysteresis = 0;
    renderer::ChunkRenderSystem chunks(cache, {9003}, nullptr, config);

    const world::ChunkCoord near_coord{0, 0, -1};
    const world::ChunkCoord middle_coord{0, 0, -2};
    const world::ChunkCoord far_coord{0, 0, -3};
    world::WorldState world;
    assert(world.chunks().set(near_coord, {1, 1, 1}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(middle_coord, {1, 1, 1}, world::VoxelCell{1, 255}));
    assert(world.chunks().set(far_coord, {1, 1, 1}, world::VoxelCell{1, 255}));
    const auto near_identity = world.chunks().find(near_coord)->identity();
    const auto middle_identity = world.chunks().find(middle_coord)->identity();
    const auto far_identity = world.chunks().find(far_coord)->identity();

    renderer::RenderCamera camera;
    camera.floating_origin.block = {16, 16, 16};
    camera.far_plane = 512.0F;
    assert(camera.update_matrices());
    const auto synchronize_until = [&](const auto& predicate) {
        for (std::size_t attempt = 0; attempt < 5'000; ++attempt) {
            assert(chunks.synchronize(world, camera));
            if (predicate()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(false && "GPU-residency-budgeted chunk renderer did not settle");
    };
    synchronize_until([&] {
        return cache.stats().resident_chunk_count == 1 &&
               chunks.stats().residency_suppressed_chunk_count == 2 &&
               chunks.stats().pending_mesh_count == 0 && chunks.stats().pending_upload_count == 0;
    });
    assert(cache.stats().resident_bytes <= config.gpu_terrain_budget_bytes);
    assert(cache.find(near_identity)->state == renderer::ChunkGpuState::resident);
    assert(cache.find(middle_identity)->residency_suppressed);
    assert(cache.find(far_identity)->residency_suppressed);
    assert(chunks.stats().memory_pressure_evicted_mesh_count >= 2);

    auto camera_block = world::chunk_local_to_block(far_coord, {16, 16, 16});
    assert(camera_block);
    camera.floating_origin.block = camera_block.value();
    camera.floating_origin.block.z -= 32;
    camera.yaw_radians = 3.14159265F;
    assert(camera.update_matrices());
    synchronize_until([&] {
        return cache.find(near_identity)->residency_suppressed &&
               (cache.find(middle_identity)->state == renderer::ChunkGpuState::resident ||
                cache.find(far_identity)->state == renderer::ChunkGpuState::resident) &&
               chunks.stats().pending_mesh_count == 0;
    });
    assert(cache.stats().resident_bytes <= config.gpu_terrain_budget_bytes);
    assert(chunks.stats().memory_pressure_evicted_mesh_count >= 3);

    chunks.shutdown();
    assert(cache.shutdown());
}

void test_chunk_gpu_cache_batches_device_local_arena_uploads() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline_resources = device.value()->live_resource_count();
    renderer::ChunkGpuCache cache(*device.value());
    renderer::ChunkGpuCacheConfig config;
    config.vertex_initial_bytes = 4096;
    config.vertex_maximum_bytes = 4096;
    config.index_initial_bytes = 4096;
    config.index_maximum_bytes = 4096;
    assert(cache.initialize(config));

    const world::ChunkIdentity first{{0, 0, 0}, 1};
    const world::ChunkIdentity second{{1, 0, 0}, 2};
    assert(cache.insert(first));
    assert(cache.insert(second));
    const std::array<renderer::terrain::GpuChunkVertex, 3> vertices{};
    const std::array<std::uint32_t, 3> indices{0, 1, 2};
    const math::Bounds3f bounds{{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}};
    const std::array<renderer::ChunkGpuMeshUpload, 2> uploads{
        renderer::ChunkGpuMeshUpload{first, 1, 1, bounds, vertices, indices, {}, {}},
        renderer::ChunkGpuMeshUpload{second, 1, 1, bounds, vertices, indices, {}, {}},
    };
    auto uploaded = cache.replace_meshes(uploads);
    assert(uploaded);
    assert(uploaded.value().uploads.size() == 2);
    assert(uploaded.value().write_count == 4);
    assert(cache.stats().upload_batch_count == 1);
    assert(cache.stats().last_upload_chunk_count == 2);
    assert(cache.stats().last_upload_write_count == 4);
    assert(cache.stats().resident_allocation_count == 4);
    assert(cache.stats().resident_buffer_count == 2);
    assert(cache.stats().resident_section_count == 2);
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    assert(cache.find(first)->mesh.vertices.buffer == cache.find(second)->mesh.vertices.buffer);
    assert(cache.find(first)->mesh.indices.buffer == cache.find(second)->mesh.indices.buffer);
    assert(cache.find(first)->mesh.vertices.offset != cache.find(second)->mesh.vertices.offset);
    assert(cache.find(first)->mesh.indices.offset != cache.find(second)->mesh.indices.offset);

    assert(cache.erase(first));
    assert(cache.erase(second));
    assert(cache.stats().vertex_arena.used_bytes == 0);
    assert(cache.stats().index_arena.used_bytes == 0);
    assert(cache.shutdown());
    assert(device.value()->live_resource_count() == baseline_resources);
}

void test_chunk_render_system_retains_rebuilds_and_culls() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    device_desc.initial_extent = {640, 640};
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);
    const auto baseline_resources = device.value()->live_resource_count();

    renderer::ChunkGpuCache cache(*device.value());
    assert(cache.initialize());
    renderer::ChunkRenderConfig config;
    config.max_chunks_meshed_per_frame = 3;
    config.max_bytes_uploaded_per_frame = 1; // Oversized meshes make progress one at a time.
    renderer::ChunkRenderSystem chunks(cache, {9001}, nullptr, config);

    constexpr std::int64_t far = 1'000'000'000;
    const world::ChunkCoord origin_coord{far, 0, -far};
    const world::ChunkCoord neighbor_coord{far + 1, 0, -far};
    const world::ChunkCoord behind_coord{far, 0, -far + 3};
    world::WorldState world;
    assert(world.chunks().set(origin_coord, {31, 8, 31}, world::VoxelCell{1, 200}));
    assert(world.chunks().set(origin_coord, {30, 8, 31}, world::VoxelCell{2, 200}));
    assert(world.chunks().set(neighbor_coord, {0, 8, 31}, world::VoxelCell{1, 180}));
    assert(world.chunks().set(behind_coord, {0, 8, 0}, world::VoxelCell{1, 160}));

    const auto origin_identity = world.chunks().find(origin_coord)->identity();
    const auto neighbor_identity = world.chunks().find(neighbor_coord)->identity();
    const auto behind_identity = world.chunks().find(behind_coord)->identity();

    auto exact_origin = world::chunk_local_to_block(origin_coord, {0, 0, 0});
    assert(exact_origin);
    renderer::RenderCamera camera;
    camera.floating_origin.block = {exact_origin.value().x + 16, exact_origin.value().y + 16,
                                    exact_origin.value().z + 64};
    camera.aspect_ratio = 1.0F;
    camera.far_plane = 256.0F;
    assert(camera.update_matrices());

    const auto synchronize_until = [&](const auto& predicate) {
        for (std::size_t attempt = 0; attempt < 5'000; ++attempt) {
            assert(chunks.synchronize(world, camera));
            if (predicate()) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(false && "asynchronous chunk renderer did not reach the expected state");
    };
    synchronize_until([&] {
        return chunks.stats().cache.resident_chunk_count == 3 &&
               chunks.stats().pending_mesh_count == 0 && chunks.stats().pending_upload_count == 0;
    });
    assert(cache.stats().resident_chunk_count == 3);
    assert(cache.stats().resident_buffer_count == 2);
    assert(cache.stats().resident_allocation_count == 6);
    assert(cache.stats().resident_section_count == 4);
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    assert(chunks.stats().pending_mesh_count == 0);
    assert(chunks.stats().pending_upload_count == 0);
    assert(chunks.stats().pooled_cpu_mesh_buffers > 0);
    assert(chunks.stats().pooled_cpu_mesh_vertex_capacity > 0);
    assert(chunks.stats().pooled_cpu_mesh_index_capacity > 0);
    assert(chunks.stats().pooled_gpu_vertex_buffers > 0);
    assert(chunks.stats().pooled_gpu_vertex_capacity > 0);

    auto draws = chunks.build_draw_list(camera);
    assert(draws.visible_chunk_count == 2);
    assert(draws.culled_chunk_count == 1);
    assert(draws.drawn_chunk_count == 2);
    assert(draws.draws.size() == 3);
    assert(draws.vertex_count > 0);
    assert(draws.index_count > 0);
    assert(std::ranges::all_of(draws.draws, [](const renderer::rhi::RenderDrawCommand& draw) {
        return draw.pipeline.value == 9001 && draw.vertex_buffer.is_valid() &&
               draw.index_buffer.is_valid() && draw.index_count > 0 &&
               std::abs(draw.camera_relative_origin.z + 64.0F) < 0.001F;
    }));
    std::vector<std::uint32_t> variation_seeds;
    std::ranges::transform(
        draws.draws, std::back_inserter(variation_seeds),
        [](const renderer::rhi::RenderDrawCommand& draw) { return draw.texture_variation_seed; });
    std::ranges::sort(variation_seeds);
    const auto unique_seeds = std::ranges::unique(variation_seeds);
    assert(std::distance(variation_seeds.begin(), unique_seeds.begin()) == 2);
    assert(std::ranges::any_of(draws.draws, [](const renderer::rhi::RenderDrawCommand& draw) {
        return draw.first_index > 0 && draw.vertex_offset > 0;
    }));

    const auto uploads_before_idle_sync = cache.stats().uploaded_chunk_count;
    assert(chunks.synchronize(world, camera));
    assert(chunks.stats().uploaded_chunk_count == 0);
    assert(cache.stats().uploaded_chunk_count == uploads_before_idle_sync);

    const auto old_origin_vertex = cache.find(origin_identity)->mesh.vertices;
    const auto old_neighbor_vertex = cache.find(neighbor_identity)->mesh.vertices;
    const auto neighbor_revision = world.chunks().find(neighbor_coord)->content_revision();
    assert(world.chunks().set(origin_coord, {31, 8, 31}, world::VoxelCell{2, 220},
                              world.dirty_regions()));
    assert(chunks.synchronize(world, camera));
    assert(chunks.build_draw_list(camera).draws.size() == 3);
    assert(cache.find(origin_identity)->mesh.vertices == old_origin_vertex);
    synchronize_until([&] {
        return !world.chunks().find(origin_coord)->dirty().contains(world::ChunkDirtyFlag::mesh) &&
               !world.chunks().find(neighbor_coord)->dirty().contains(world::ChunkDirtyFlag::mesh);
    });
    assert(cache.find(origin_identity)->mesh.vertices != old_origin_vertex);
    assert(cache.find(neighbor_identity)->mesh.vertices != old_neighbor_vertex);
    assert(cache.find(origin_identity)->resident_content_revision ==
           world.chunks().find(origin_coord)->content_revision());
    assert(cache.find(neighbor_identity)->resident_content_revision == neighbor_revision);
    assert(!world.chunks().find(origin_coord)->dirty().contains(world::ChunkDirtyFlag::mesh));
    assert(!world.chunks().find(neighbor_coord)->dirty().contains(world::ChunkDirtyFlag::mesh));
    assert(device.value()->live_resource_count() == baseline_resources + 2);

    const auto intermediate_revision_before = world.chunks().find(origin_coord)->content_revision();
    assert(world.chunks().set(origin_coord, {30, 8, 31}, world::VoxelCell{1, 220},
                              world.dirty_regions()));
    const auto intermediate_revision = world.chunks().find(origin_coord)->content_revision();
    assert(intermediate_revision > intermediate_revision_before);
    assert(chunks.synchronize(world, camera)); // Queues the intermediate revision.
    assert(cache.find(origin_identity)->resident_content_revision != intermediate_revision);
    assert(world.chunks().set(origin_coord, {30, 8, 31}, world::VoxelCell{2, 220},
                              world.dirty_regions()));
    const auto newest_revision = world.chunks().find(origin_coord)->content_revision();
    assert(newest_revision > intermediate_revision);
    synchronize_until([&] {
        const auto* entry = cache.find(origin_identity);
        assert(entry != nullptr);
        assert(entry->resident_content_revision != intermediate_revision);
        return entry->resident_content_revision == newest_revision &&
               !world.chunks().find(origin_coord)->dirty().contains(world::ChunkDirtyFlag::mesh);
    });

    const world::ChunkCoord empty_coord{far, 3, -far};
    auto& empty = world.chunks().get_or_create(empty_coord);
    const auto empty_identity = empty.identity();
    synchronize_until([&] {
        const auto* entry = cache.find(empty_identity);
        return entry != nullptr && entry->state == renderer::ChunkGpuState::resident;
    });
    assert(cache.find(empty_identity) != nullptr);
    assert(cache.find(empty_identity)->state == renderer::ChunkGpuState::resident);
    assert(!cache.find(empty_identity)->has_drawable_mesh());
    assert(cache.stats().empty_chunk_count == 1);
    assert(device.value()->live_resource_count() == baseline_resources + 2);

    assert(world.chunks().erase(behind_coord));
    const std::array<world::ChunkIdentity, 1> evictions{behind_identity};
    assert(chunks.process_chunk_evictions(evictions));
    assert(!cache.contains(behind_identity));
    assert(device.value()->live_resource_count() == baseline_resources + 2);

    assert(world.chunks().set(empty_coord, {2, 2, 2}, world::VoxelCell{1, 255},
                              world.dirty_regions()));
    assert(chunks.synchronize(world, camera)); // Leaves work for the old generation in flight.
    assert(world.chunks().erase(empty_coord));
    auto& reloaded = world.chunks().get_or_create(empty_coord);
    assert(reloaded.set({1, 1, 1}, world::VoxelCell{3, 255}));
    assert(reloaded.identity() != empty_identity);
    assert(chunks.synchronize(world, camera));
    assert(!cache.contains(empty_identity));
    assert(cache.contains(reloaded.identity()));
    synchronize_until([&] {
        const auto* entry = cache.find(reloaded.identity());
        return entry != nullptr && entry->state == renderer::ChunkGpuState::resident &&
               entry->resident_content_revision == reloaded.content_revision();
    });
    assert(!cache.contains(empty_identity));

    assert(cache.clear());
    assert(device.value()->live_resource_count() == baseline_resources + 2);
    chunks.shutdown();
    assert(cache.shutdown());
    assert(device.value()->live_resource_count() == baseline_resources);
}

void test_renderer_frontend_submits_headless_frames() {
    using namespace heartstead;
    renderer::rhi::RenderDeviceDesc device_desc;
    device_desc.backend = renderer::rhi::RenderBackend::headless;
    device_desc.initial_extent = {640, 360};
    auto device = renderer::rhi::create_render_device(device_desc);
    assert(device);

    // Headless validates the SPIR-V header and all retained frame resources without invoking a
    // native shader compiler.
    const std::vector<std::uint32_t> test_spirv{0x07230203, 0x00010000, 0, 1, 0};
    renderer::RendererInitDesc init;
    init.device = std::move(device).value();
    init.sky_vertex_spirv = test_spirv;
    init.sky_fragment_spirv = test_spirv;
    init.terrain_vertex_spirv = test_spirv;
    init.terrain_fragment_spirv = test_spirv;
    init.static_mesh_vertex_spirv = test_spirv;
    init.static_mesh_fragment_spirv = test_spirv;
    init.debug_vertex_spirv = test_spirv;
    init.debug_fragment_spirv = test_spirv;
    init.ui_vertex_spirv = test_spirv;
    init.ui_fragment_spirv = test_spirv;
    init.development_shader_hot_reload = true;
    init.chunk_config.max_chunks_meshed_per_frame = 2;
    init.chunk_config.max_bytes_uploaded_per_frame = 1024 * 1024;
    init.scene_render_config.maximum_instances_per_frame = 2;
    init.scene_render_config.maximum_skin_matrices_per_frame = 4;
    renderer::materials::TerrainTextureAsset terrain_texture;
    terrain_texture.logical_id = "base:textures/voxels/test_grass.png";
    terrain_texture.image.width = 2;
    terrain_texture.image.height = 2;
    terrain_texture.image.rgba8 = {
        32, 96, 24, 255, 48, 128, 32, 255, 64, 144, 40, 255, 80, 160, 48, 255,
    };
    init.terrain_material_assets.textures.push_back(std::move(terrain_texture));
    renderer::materials::TerrainTextureAsset terrain_texture_variant;
    terrain_texture_variant.logical_id = "base:textures/voxels/test_grass_variant.png";
    terrain_texture_variant.image.width = 2;
    terrain_texture_variant.image.height = 2;
    terrain_texture_variant.image.rgba8 = {
        24, 88, 16, 255, 40, 120, 24, 255, 56, 136, 32, 255, 72, 152, 40, 255,
    };
    init.terrain_material_assets.textures.push_back(std::move(terrain_texture_variant));
    renderer::materials::TerrainVoxelMaterialAsset terrain_material;
    terrain_material.voxel_type = 1;
    for (auto& face : terrain_material.face_textures) {
        face.push_back(0);
    }
    terrain_material
        .face_textures[renderer::voxel_material_face_index(renderer::VoxelMaterialFace::west)] = {
        0, 1};
    terrain_material
        .face_textures[renderer::voxel_material_face_index(renderer::VoxelMaterialFace::east)] = {
        1, 0};
    terrain_material.base_color = {0.75F, 1.0F, 0.75F, 1.0F};
    terrain_material.roughness = 0.9F;
    init.terrain_material_assets.materials.push_back(terrain_material);

    renderer::Renderer retained_renderer;
    assert(retained_renderer.initialize(std::move(init)));
    assert(retained_renderer.is_owner_thread());
    bool worker_reports_owner = true;
    std::thread ownership_probe(
        [&] { worker_reports_owner = retained_renderer.is_owner_thread(); });
    ownership_probe.join();
    assert(!worker_reports_owner);
    assert(retained_renderer.is_initialized());
    assert(retained_renderer.device() != nullptr);
    assert(retained_renderer.fallback_resources().is_valid());
    const auto terrain_array = retained_renderer.describe_terrain_texture();
    assert(terrain_array);
    assert(terrain_array->array_layers == 11);
    const auto voxel_material = retained_renderer.describe_voxel_material(1);
    assert(voxel_material);
    assert((std::array<std::uint32_t, renderer::voxel_material_face_count>{
                voxel_material->face_texture_starts[0], voxel_material->face_texture_starts[1],
                voxel_material->face_texture_starts[2], voxel_material->face_texture_starts[3],
                voxel_material->face_texture_starts[4], voxel_material->face_texture_starts[5]} ==
            std::array<std::uint32_t, renderer::voxel_material_face_count>{7, 9, 7, 7, 7, 7}));
    assert((std::array<std::uint32_t, renderer::voxel_material_face_count>{
                voxel_material->face_texture_counts[0], voxel_material->face_texture_counts[1],
                voxel_material->face_texture_counts[2], voxel_material->face_texture_counts[3],
                voxel_material->face_texture_counts[4], voxel_material->face_texture_counts[5]} ==
            std::array<std::uint32_t, renderer::voxel_material_face_count>{2, 2, 1, 1, 1, 1}));
    assert(voxel_material->base_color == terrain_material.base_color);
    assert(voxel_material->roughness == terrain_material.roughness);
    const auto initialized_resource_count = retained_renderer.device()->live_resource_count();
    // Ten shader modules, eleven prewarmed pipelines, sky geometry, four fallback textures,
    // terrain/UI plus color/data surface arrays, one shared sampler, one material-table buffer,
    // static/morph arenas, instance/skin/morph rings, and buffered debug/UI geometry.
    assert(initialized_resource_count == 46);

    assets::ModelAsset material_model;
    material_model.vertices.resize(3);
    material_model.vertices[0].position = {-0.5F, 0.0F, 0.0F};
    material_model.vertices[1].position = {0.5F, 0.0F, 0.0F};
    material_model.vertices[2].position = {0.0F, 1.0F, 0.0F};
    material_model.indices = {0, 1, 2};
    material_model.nodes.push_back({"root", assets::no_model_index, {}});
    material_model.primitives.push_back({"triangle", 0, 3, 0, 3, 0, assets::no_model_index, 0});
    material_model.images.push_back(
        {"base_color",
         2,
         2,
         {16, 32, 64, 255, 32, 64, 128, 255, 64, 128, 192, 255, 128, 192, 224, 128}});
    assets::ModelMaterial source_material;
    source_material.name = "paint";
    source_material.base_color_factor = {0.25F, 0.5F, 0.75F, 0.8F};
    source_material.base_color_texture.image = 0;
    source_material.alpha_mode = assets::ModelAlphaMode::mask;
    source_material.alpha_cutoff = 0.375F;
    source_material.double_sided = true;
    source_material.unlit = true;
    material_model.materials.push_back(source_material);
    material_model.bounds = {{-0.5F, 0.0F, 0.0F}, {0.5F, 1.0F, 0.0F}};
    auto model_materials = retained_renderer.create_model_materials(
        "base:models/props/color_contract.gltf", material_model);
    assert(model_materials);
    assert(model_materials.value().size() == 1);
    const auto texture = retained_renderer.describe_surface_texture();
    assert(texture);
    assert(texture->color_space == renderer::TextureColorSpace::srgb);
    assert(texture->array_layers == 5);
    const auto material =
        retained_renderer.describe_material(model_materials.value().front().material);
    assert(material);
    assert(material->base_color == source_material.base_color_factor);
    assert(material->alpha_cutoff == source_material.alpha_cutoff);
    assert(material->surface_texture == 4);
    assert((static_cast<std::uint32_t>(material->flags) &
            static_cast<std::uint32_t>(renderer::VoxelMaterialFlags::unlit)) != 0U);
    assert(model_materials.value().front().layer == renderer::RenderLayer::alpha_tested);
    assert(renderer::any(model_materials.value().front().flags &
                         renderer::RenderObjectFlags::two_sided));
    const auto gpu_material = renderer::gpu_surface_material(*material);
    assert(std::ranges::equal(gpu_material.base_color, source_material.base_color_factor));
    auto cached_model_materials = retained_renderer.create_model_materials(
        "base:models/props/color_contract.gltf", material_model);
    assert(cached_model_materials);
    assert(cached_model_materials.value().front().material ==
           model_materials.value().front().material);
    assert(retained_renderer.describe_surface_texture()->array_layers == 5);

    renderer::rhi::RenderEnvironmentData invalid_environment;
    invalid_environment.fog_end = invalid_environment.fog_start;
    assert(!retained_renderer.set_environment(invalid_environment));
    renderer::rhi::RenderEnvironmentData environment;
    environment.sun_intensity = 0.75F;
    environment.fog_start = 96.0F;
    environment.fog_end = 160.0F;
    assert(retained_renderer.set_environment(environment));
    world::ChunkLightSystemStats lighting_stats;
    lighting_stats.last_solve_ms = 1.25;
    lighting_stats.last_apply_ms = 0.5;
    lighting_stats.snapshot_pending_cell_count = 2048;
    lighting_stats.last_sunlight_queue_visits = 300;
    lighting_stats.last_block_light_queue_visits = 40;
    lighting_stats.changed_chunks_this_update = 2;
    lighting_stats.stale_results = 3;
    lighting_stats.failed_results = 2;
    lighting_stats.apply_budget_overruns = 1;
    retained_renderer.set_voxel_lighting_stats(lighting_stats);

    auto invalid_spirv = test_spirv;
    invalid_spirv[0] = 0;
    assert(!retained_renderer.reload_terrain_shaders(invalid_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(retained_renderer.reload_terrain_shaders(test_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(!retained_renderer.reload_static_mesh_shaders(invalid_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(retained_renderer.reload_static_mesh_shaders(test_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(!retained_renderer.reload_debug_shaders(invalid_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(retained_renderer.reload_debug_shaders(test_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(!retained_renderer.reload_ui_shaders(invalid_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);
    assert(retained_renderer.reload_ui_shaders(test_spirv, test_spirv));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count);

    world::WorldState world;
    assert(world.chunks().set({0, 0, 0}, {4, 4, 4}, world::VoxelCell{1, 255}));
    const auto identity = world.chunks().find({0, 0, 0})->identity();
    world::ChunkStreamLoadReport load_report;
    load_report.coord = identity.coordinate;
    load_report.identity = identity;
    renderer::ChunkRenderUpdate load_update;
    load_update.kind = renderer::ChunkRenderUpdateKind::loaded;
    load_update.load = load_report;
    const std::array renderer_loads{load_update};
    assert(retained_renderer.process_world_render_updates(renderer_loads));
    renderer::RenderCamera camera;
    camera.floating_origin.block = {16, 16, 64};
    assert(camera.set_aspect_ratio(640.0F / 360.0F));

    for (std::size_t attempt = 0;
         attempt < 5'000 && retained_renderer.chunk_stats().cache.resident_chunk_count != 1;
         ++attempt) {
        assert(retained_renderer.synchronize_chunks(world, camera));
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(retained_renderer.chunk_stats().cache.resident_chunk_count == 1);
    assert(retained_renderer.chunk_stats().cache.resident_buffer_count == 2);
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count + 2);

    renderer::RenderFrameInput frame_input;
    frame_input.camera = camera;
    frame_input.simulation_alpha = 1.0F;
    frame_input.delta_seconds = 1.0F / 60.0F;
    auto first_frame_result = retained_renderer.render_frame(frame_input);
    assert(first_frame_result);
    const auto& first_frame = first_frame_result.value().frame;
    assert(first_frame_result.value().renderer.frame_index == first_frame.frame_index);
    assert(first_frame.draw_count == 2);
    auto invalid_frame_input = frame_input;
    invalid_frame_input.simulation_alpha = 2.0F;
    assert(!retained_renderer.render_frame(invalid_frame_input));
    assert(first_frame.render_pass_count == 8);
    assert(first_frame.opaque_terrain_draw_count == 1);
    assert(first_frame.alpha_tested_terrain_draw_count == 0);
    assert(first_frame.transparent_terrain_draw_count == 0);
    assert(first_frame.indexed_draw_count == 2);
    assert(first_frame.pipeline_bind_count == 2);
    assert(first_frame.clear_color.red == environment.fog_color.x);
    assert(first_frame.clear_color.green == environment.fog_color.y);
    assert(first_frame.clear_color.blue == environment.fog_color.z);
    assert(first_frame.total_indices > 0);
    assert(retained_renderer.chunk_stats().visible_chunk_count == 1);
    assert(retained_renderer.chunk_stats().culled_chunk_count == 0);
    const auto& renderer_stats = retained_renderer.stats();
    assert(renderer_stats.cpu_frame_ms > 0.0);
    assert(renderer_stats.chunk_synchronization_ms > 0.0);
    assert(renderer_stats.render_extraction_ms > 0.0);
    assert(renderer_stats.draw_list_ms > 0.0);
    assert(renderer_stats.command_build_ms > 0.0);
    assert(renderer_stats.meshing_ms > 0.0);
    assert(renderer_stats.upload_ms > 0.0);
    assert(renderer_stats.command_recording_ms > 0.0);
    assert(renderer_stats.cpu_frame_ms >= renderer_stats.chunk_synchronization_ms);
    assert(!renderer_stats.gpu_timing_valid);
    assert(!renderer_stats.gpu_upload_timing_valid);
    assert(renderer_stats.gpu_wait_ms == 0.0);
    assert(renderer_stats.loaded_chunks == 1);
    assert(renderer_stats.mesh_pending_chunks == 0);
    assert(renderer_stats.upload_pending_chunks == 0);
    assert(renderer_stats.mesh_failures == 0);
    assert(renderer_stats.upload_failures == 0);
    assert(renderer_stats.stale_mesh_results == 0);
    assert(renderer_stats.resident_chunks == 1);
    assert(renderer_stats.visible_chunks == 1);
    assert(renderer_stats.drawn_chunks == 1);
    assert(renderer_stats.draw_calls == 2);
    assert(renderer_stats.pipeline_switches == 2);
    assert(renderer_stats.resident_textures == 8);
    assert(renderer_stats.runtime_materials == 257);
    assert(renderer_stats.resident_pipelines == 12);
    assert(renderer_stats.resident_texture_bytes > 0);
    assert(renderer_stats.vertices > 0);
    assert(renderer_stats.triangles > 0);
    assert(renderer_stats.resident_mesh_bytes > 0);
    assert(renderer_stats.uploaded_bytes_this_frame > 0);
    assert(renderer_stats.voxel_relight_solve_ms == 1.25);
    assert(renderer_stats.voxel_relight_apply_ms == 0.5);
    assert(renderer_stats.voxel_relight_backlog_cells == 2048);
    assert(renderer_stats.voxel_relight_visited_cells == 340);
    assert(renderer_stats.voxel_relight_changed_chunks == 2);
    assert(renderer_stats.voxel_relight_stale_results == 3);
    assert(renderer_stats.voxel_relight_failed_results == 2);
    assert(renderer_stats.voxel_relight_apply_budget_overruns == 1);
    const auto formatted_stats = renderer::format_renderer_stats(renderer_stats);
    assert(formatted_stats.contains("relight_cells=340/2048"));
    assert(formatted_stats.contains("relight_failed=2"));
    assert(formatted_stats.contains("chunk_failed=0/0"));

    constexpr std::array<renderer::GpuStaticMeshVertex, 3> object_vertices{{
        {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 0.0F}},
        {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {1.0F, 0.0F}},
        {{0.0F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.5F, 1.0F}},
    }};
    constexpr std::array<std::uint32_t, 3> object_indices{0, 1, 2};
    auto object_mesh =
        retained_renderer.create_static_mesh({"frontend_triangle",
                                              object_vertices,
                                              object_indices,
                                              {{-0.5F, -0.5F, 0.0F}, {0.5F, 0.5F, 0.0F}}});
    assert(object_mesh);
    auto object_anchor = world::WorldPosition::from_anchor(camera.floating_origin.block, {});
    assert(object_anchor);
    renderer::RenderObjectProxy object;
    object.anchor = object_anchor.value();
    object.previous_transform.position = {0.0F, 0.0F, -5.0F};
    object.current_transform = object.previous_transform;
    object.mesh = object_mesh.value();
    object.material = retained_renderer.fallback_material();
    object.local_bounds = {{-0.5F, -0.5F, 0.0F}, {0.5F, 0.5F, 0.0F}};
    auto object_id = retained_renderer.create_object(object);
    assert(object_id);
    object.previous_transform.position.x = 1.25F;
    object.current_transform = object.previous_transform;
    auto second_object_id = retained_renderer.create_object(object);
    assert(second_object_id);
    object.previous_transform.position.x = -1.25F;
    object.current_transform = object.previous_transform;
    auto third_object_id = retained_renderer.create_object(object);
    assert(third_object_id);
    assert(retained_renderer.debug_renderer() != nullptr);
    assert(retained_renderer.debug_renderer()->submit_axes(object_anchor.value(), 1.0F));
    assert(retained_renderer.debug_renderer()->submit_text(
        {object_anchor.value(), "frontend debug label"}));
    assert(retained_renderer.ui_renderer() != nullptr);
    renderer::UiQuadDesc ui_quad;
    ui_quad.minimum_pixels = {8.0F, 8.0F};
    ui_quad.maximum_pixels = {120.0F, 42.0F};
    ui_quad.color = {0.3F, 0.7F, 1.0F, 0.8F};
    ui_quad.scissor_enabled = true;
    ui_quad.scissor = {4, 4, 160, 64};
    assert(retained_renderer.ui_renderer()->submit_quad(ui_quad));
    assert(retained_renderer.ui_renderer()->submit_text(
        {{12.0F, 48.0F}, "FPS 144", 8.0F, {1.0F, 1.0F, 1.0F, 1.0F}}));
    auto instanced_frame = retained_renderer.render(camera, 0.5F);
    assert(instanced_frame);
    assert(instanced_frame.value().draw_count == 6);
    assert(instanced_frame.value().indexed_draw_count == 6);
    assert(instanced_frame.value().pipeline_bind_count == 5);
    assert(retained_renderer.scene_stats().scene.visible_objects == 3);
    assert(retained_renderer.scene_stats().submitted_instances == 2);
    assert(retained_renderer.scene_stats().draw_calls == 1);
    assert(retained_renderer.scene_stats().dropped_instances == 1);
    assert(retained_renderer.scene_stats().uploaded_instance_bytes ==
           sizeof(renderer::GpuObjectInstance) * 2);
    assert(retained_renderer.debug_renderer()->stats().submitted_lines == 3);
    assert(retained_renderer.debug_text_labels().size() == 1);
    assert(retained_renderer.stats().debug_lines == 3);
    assert(retained_renderer.stats().debug_draw_calls == 1);
    assert(retained_renderer.stats().debug_labels == 1);
    assert(retained_renderer.stats().ui_vertices == 32);
    assert(retained_renderer.stats().ui_draw_calls == 2);
    assert(retained_renderer.stats().ui_clipped_draw_calls == 1);
    assert(retained_renderer.stats().ui_glyphs == 7);

    renderer::RenderSceneUpdate remove_object;
    remove_object.kind = renderer::RenderSceneUpdateKind::remove_object;
    remove_object.object_id = object_id.value();
    auto remove_second_object = remove_object;
    remove_second_object.object_id = second_object_id.value();
    auto remove_third_object = remove_object;
    remove_third_object.object_id = third_object_id.value();
    const std::array removals{remove_object, remove_second_object, remove_third_object};
    assert(retained_renderer.apply_scene_updates(removals));
    assert(retained_renderer.release_static_mesh(object_mesh.value()));
    auto terrain_only_frame = retained_renderer.render(camera);
    assert(terrain_only_frame);
    assert(terrain_only_frame.value().draw_count == 2);

    assets::ModelAsset animated_model;
    animated_model.vertices = {
        {{-0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
        {{0.5F, -0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
        {{0.0F, 0.5F, 0.0F}, {0.0F, 0.0F, 1.0F}, {}, {0, 0, 0, 0}, {1.0F, 0.0F, 0.0F, 0.0F}},
    };
    animated_model.indices = {0, 1, 2};
    animated_model.nodes = {
        {"mesh", assets::no_model_index, {}},
        {"joint", 0, {}},
    };
    animated_model.primitives = {{"animated_body", 0, 3, 0, 3, 0, 0}};
    animated_model.nodes[0].morph_weights = {0.0F};
    assets::ModelMorphTarget morph_target;
    morph_target.position_deltas = {
        {0.0F, 0.1F, 0.0F},
        {0.0F, 0.1F, 0.0F},
        {0.0F, 0.1F, 0.0F},
    };
    animated_model.primitives[0].morph_targets.push_back(std::move(morph_target));
    animated_model.skins = {{"animated_skin", 1, {1}, {math::Mat4f::identity()}}};
    animated_model.bounds = {{-0.5F, -0.5F, 0.0F}, {0.5F, 0.5F, 0.0F}};
    assert(assets::validate_model_asset(animated_model));
    auto animated_mesh =
        retained_renderer.create_model_primitive("frontend_animated_body", animated_model, 0);
    assert(animated_mesh);
    renderer::RenderSkinPaletteProxy skin_palette;
    skin_palette.joint_matrices = {math::translation_matrix({0.0F, 0.25F, 0.0F})};
    auto skin_palette_id = retained_renderer.create_skin_palette(std::move(skin_palette));
    assert(skin_palette_id);
    renderer::RenderObjectProxy animated_object;
    animated_object.anchor = object_anchor.value();
    animated_object.previous_transform.position = {0.0F, 0.0F, -5.0F};
    animated_object.current_transform = animated_object.previous_transform;
    animated_object.mesh = animated_mesh.value();
    animated_object.material = retained_renderer.fallback_material();
    animated_object.local_bounds = animated_model.bounds;
    animated_object.skin_palette = skin_palette_id.value();
    animated_object.morph_weights = {0.5F};
    auto animated_object_id = retained_renderer.create_object(animated_object);
    assert(animated_object_id);
    animated_object.previous_transform.position.x = 1.0F;
    animated_object.current_transform = animated_object.previous_transform;
    auto second_animated_object_id = retained_renderer.create_object(animated_object);
    assert(second_animated_object_id);
    auto animated_frame = retained_renderer.render(camera);
    assert(animated_frame);
    assert(animated_frame.value().draw_count == 3);
    assert(retained_renderer.scene_stats().submitted_instances == 2);
    assert(retained_renderer.scene_stats().submitted_skin_palettes == 1);
    assert(retained_renderer.scene_stats().submitted_skin_matrices == 1);
    assert(retained_renderer.scene_stats().uploaded_skin_matrix_bytes == sizeof(math::Mat4f));
    assert(retained_renderer.scene_stats().uploaded_morph_weight_bytes == sizeof(float) * 2U);
    assert(retained_renderer.stats().retained_skin_palettes == 1);
    assert(retained_renderer.stats().submitted_skin_matrices == 1);

    renderer::RenderSceneUpdate remove_animated_object;
    remove_animated_object.kind = renderer::RenderSceneUpdateKind::remove_object;
    remove_animated_object.object_id = animated_object_id.value();
    auto remove_second_animated_object = remove_animated_object;
    remove_second_animated_object.object_id = second_animated_object_id.value();
    renderer::RenderSceneUpdate remove_skin_palette;
    remove_skin_palette.kind = renderer::RenderSceneUpdateKind::remove_skin_palette;
    remove_skin_palette.skin_palette_id = skin_palette_id.value();
    const std::array animated_removals{remove_animated_object, remove_second_animated_object,
                                       remove_skin_palette};
    assert(retained_renderer.apply_scene_updates(animated_removals));
    assert(retained_renderer.release_static_mesh(animated_mesh.value()));

    const auto uploaded_before_resize = retained_renderer.chunk_stats().cache.uploaded_chunk_count;
    assert(retained_renderer.resize({800, 400}));
    assert(camera.set_aspect_ratio(2.0F));
    assert(retained_renderer.synchronize_chunks(world, camera));
    assert(retained_renderer.chunk_stats().uploaded_chunk_count == 0);
    assert(retained_renderer.chunk_stats().cache.uploaded_chunk_count == uploaded_before_resize);
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count + 2);
    auto resized_frame = retained_renderer.render(camera);
    assert(resized_frame);
    assert(resized_frame.value().extent.width == 800);
    assert(resized_frame.value().extent.height == 400);
    assert(resized_frame.value().draw_count == 2);

    assert(world.chunks().erase(identity.coordinate));
    renderer::ChunkRenderUpdate eviction_update;
    eviction_update.kind = renderer::ChunkRenderUpdateKind::evicted;
    eviction_update.identity = identity;
    const std::array renderer_evictions{eviction_update};
    assert(retained_renderer.process_world_render_updates(renderer_evictions));
    assert(retained_renderer.device()->live_resource_count() == initialized_resource_count + 2);
    auto empty_frame = retained_renderer.render(camera);
    assert(empty_frame);
    assert(empty_frame.value().draw_count == 1);

    assert(retained_renderer.shutdown());
    assert(!retained_renderer.is_initialized());
    assert(retained_renderer.device() == nullptr);
}

} // namespace

int main() {
    test_scoped_cpu_timing_zones();
    test_frustum_aabb_culling();
    test_frame_builder_preserves_terrain_phase_commands();
    test_chunk_gpu_cache_lifecycle();
    test_chunk_draws_use_phase_pipelines_and_transparent_ordering();
    test_chunk_gpu_cache_batches_device_local_arena_uploads();
    test_chunk_render_distances_and_residency_hysteresis();
    test_chunk_gpu_residency_budget_prefers_near_visible_meshes();
    test_chunk_render_system_retains_rebuilds_and_culls();
    test_renderer_frontend_submits_headless_frames();
    return 0;
}
