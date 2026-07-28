#include "engine/renderer/benchmark/benchmark_scene.hpp"
#include "engine/renderer/benchmark/benchmark_statistics.hpp"
#include "engine/world/fluids/chunk_fluid_system.hpp"

#include <array>
#include <cassert>
#include <cmath>
#include <string>

namespace {

void test_benchmark_statistics() {
    using namespace heartstead::renderer;
    benchmark::BenchmarkRunMetadata metadata;
    metadata.scene = "deterministic-test";
    metadata.seed = 77;
    metadata.backend = "headless";
    metadata.mesher = "greedy";
    metadata.initial_width = 1920;
    metadata.initial_height = 1080;
    metadata.chunk_radius = 16;
    metadata.warmup_frames = 120;
    metadata.measured_frames = 4;
    benchmark::BenchmarkRecorder recorder(metadata);
    constexpr std::array frame_times{1.0, 2.0, 3.0, 100.0};
    for (std::size_t index = 0; index < frame_times.size(); ++index) {
        RendererStats stats;
        stats.frame_index = index;
        stats.cpu_frame_ms = frame_times[index];
        stats.chunk_synchronization_ms = static_cast<double>(index) * 2.0;
        stats.command_recording_ms = 0.125 * static_cast<double>(index);
        stats.meshing_ms = static_cast<double>(index);
        stats.upload_ms = 0.5;
        stats.gpu_wait_ms = 0.25;
        stats.voxel_relight_solve_ms = 0.25 * static_cast<double>(index);
        stats.voxel_relight_apply_ms = 0.1 * static_cast<double>(index);
        stats.voxel_relight_backlog_cells = index * 10U;
        stats.voxel_relight_visited_cells = index * 100U;
        stats.voxel_relight_changed_chunks = static_cast<std::uint32_t>(index);
        stats.voxel_relight_stale_results = index / 2U;
        stats.voxel_relight_apply_budget_overruns = index / 3U;
        stats.voxel_fluid_snapshot_ms = 0.05 * static_cast<double>(index);
        stats.voxel_fluid_simulation_ms = 0.5 * static_cast<double>(index);
        stats.voxel_fluid_apply_ms = 0.2 * static_cast<double>(index);
        stats.voxel_fluid_active_cells = index * 1'000U;
        stats.voxel_fluid_processed_cells = index * 2'000U;
        stats.voxel_fluid_changed_chunks = static_cast<std::uint32_t>(index);
        stats.voxel_fluid_budget_exhaustions = index / 2U;
        stats.voxel_fluid_apply_budget_overruns = index / 3U;
        stats.uploaded_bytes_this_frame = 16;
        if (index >= 2) {
            stats.gpu_timing_valid = true;
            stats.gpu_frame_ms = static_cast<double>(index);
            stats.gpu_opaque_terrain_ms = 0.5;
            stats.gpu_alpha_tested_terrain_ms = 0.25;
            stats.gpu_transparent_terrain_ms = 0.125;
        }
        if (index >= 1) {
            stats.gpu_upload_timing_valid = true;
            stats.gpu_upload_submission_serial = index + 10;
            stats.gpu_upload_ms = 0.25 * static_cast<double>(index);
        }
        recorder.record(stats);
    }

    const auto summary = recorder.summarize();
    assert(summary.scene == "deterministic-test");
    assert(summary.seed == 77);
    assert(summary.sample_count == 4);
    assert(summary.gpu_sample_count == 2);
    assert(summary.gpu_upload_sample_count == 3);
    assert(std::abs(summary.mean_gpu_upload_ms - 0.5) < 0.0001);
    assert(std::abs(summary.mean_gpu_opaque_terrain_ms - 0.5) < 0.0001);
    assert(std::abs(summary.mean_gpu_alpha_tested_terrain_ms - 0.25) < 0.0001);
    assert(std::abs(summary.mean_gpu_transparent_terrain_ms - 0.125) < 0.0001);
    assert(std::abs(summary.median_frame_ms - 2.5) < 0.0001);
    assert(std::abs(summary.p95_frame_ms - 85.45) < 0.0001);
    assert(std::abs(summary.p99_frame_ms - 97.09) < 0.0001);
    assert(std::abs(summary.one_percent_low_fps - 10.0) < 0.0001);
    assert(std::abs(summary.point_one_percent_low_fps - 10.0) < 0.0001);
    assert(summary.maximum_frame_ms == 100.0);
    assert(summary.total_uploaded_bytes == 64);
    assert(std::abs(summary.median_voxel_relight_solve_ms - 0.375) < 0.0001);
    assert(std::abs(summary.p95_voxel_relight_solve_ms - 0.7125) < 0.0001);
    assert(std::abs(summary.median_voxel_relight_apply_ms - 0.15) < 0.0001);
    assert(std::abs(summary.p95_voxel_relight_apply_ms - 0.285) < 0.0001);
    assert(summary.maximum_voxel_relight_backlog_cells == 30);
    assert(summary.maximum_voxel_relight_visited_cells == 300);
    assert(summary.total_voxel_relight_changed_chunks == 6);
    assert(summary.final_voxel_relight_stale_results == 1);
    assert(summary.final_voxel_relight_apply_budget_overruns == 1);
    assert(std::abs(summary.median_voxel_fluid_snapshot_ms - 0.075) < 0.0001);
    assert(std::abs(summary.p95_voxel_fluid_simulation_ms - 1.425) < 0.0001);
    assert(std::abs(summary.median_voxel_fluid_apply_ms - 0.3) < 0.0001);
    assert(summary.maximum_voxel_fluid_active_cells == 3'000);
    assert(summary.maximum_voxel_fluid_processed_cells == 6'000);
    assert(summary.total_voxel_fluid_changed_chunks == 6);
    assert(summary.final_voxel_fluid_budget_exhaustions == 1);
    assert(summary.final_voxel_fluid_apply_budget_overruns == 1);
    assert(summary.slowest_frame.frame_index == 3);
    assert(std::abs(summary.mean_chunk_synchronization_ms - 3.0) < 0.0001);
    assert(std::abs(summary.mean_command_recording_ms - 0.1875) < 0.0001);
    assert(recorder.metadata().initial_width == 1920);
    assert(recorder.to_json().find("\"schema\": \"heartstead.renderer_benchmark.v1\"") !=
           std::string::npos);
    assert(recorder.to_json().find("\"warmup_frames\": 120") != std::string::npos);
    assert(recorder.to_json().find("\"p99_frame_ms\": 97.090000") != std::string::npos);
    assert(recorder.to_json().find("\"frames\": [") != std::string::npos);
    assert(recorder.to_json().find("\"gpu_upload_ms\": 0.750000") != std::string::npos);
    assert(recorder.to_json().find("\"gpu_alpha_tested_ms\": 0.250000") != std::string::npos);
    assert(recorder.to_json().find("\"p95_voxel_relight_solve_ms\": 0.712500") !=
           std::string::npos);
    assert(recorder.to_json().find("\"voxel_relight_visited_cells\": 300") != std::string::npos);
    assert(recorder.to_json().find("\"p95_voxel_fluid_simulation_ms\": 1.425000") !=
           std::string::npos);
    assert(recorder.to_json().find("\"voxel_fluid_processed_cells\": 6000") !=
           std::string::npos);
    assert(recorder.to_json().find("\"slowest_frame\": {\"frame\": 3") != std::string::npos);
    assert(
        recorder.to_csv().find(
            "scene,seed,backend,mesher,initial_width,initial_height,chunk_radius,warmup_frames") ==
        0);
    assert(recorder.to_csv().find("\"headless\",\"greedy\",1920,1080,16,120,4") !=
           std::string::npos);
    assert(benchmark::format_benchmark_summary(summary).find("0.1%low=10.000fps") !=
           std::string::npos);
    assert(benchmark::format_benchmark_summary(summary).find("relight=0.375/0.71") !=
           std::string::npos);
    assert(benchmark::format_benchmark_summary(summary).find("fluid=0.750/1.425") !=
           std::string::npos);
}

void test_low_fps_windows_are_distinct() {
    using namespace heartstead::renderer;
    benchmark::BenchmarkRecorder recorder("low-window-test", 1);
    for (std::uint64_t frame = 0; frame < 1'000; ++frame) {
        RendererStats stats;
        stats.frame_index = frame;
        stats.cpu_frame_ms = frame < 990 ? 10.0 : (frame == 999 ? 200.0 : 100.0);
        recorder.record(stats);
    }
    const auto summary = recorder.summarize();
    assert(std::abs(summary.one_percent_low_fps - (1'000.0 / 110.0)) < 0.0001);
    assert(std::abs(summary.point_one_percent_low_fps - 5.0) < 0.0001);
    assert(summary.maximum_frame_ms == 200.0);
}

void test_all_deterministic_scenes_construct() {
    using namespace heartstead::renderer::benchmark;
    constexpr std::array kinds{
        BenchmarkSceneKind::flat_terrain,          BenchmarkSceneKind::mountainous_terrain,
        BenchmarkSceneKind::dense_caves,           BenchmarkSceneKind::checkerboard_geometry,
        BenchmarkSceneKind::forest_cross_planes,   BenchmarkSceneKind::rapid_voxel_edits,
        BenchmarkSceneKind::high_speed_flythrough, BenchmarkSceneKind::chunk_load_unload_churn,
        BenchmarkSceneKind::large_coordinates,     BenchmarkSceneKind::resize_minimize_stress,
        BenchmarkSceneKind::active_water,
    };
    for (const auto kind : kinds) {
        BenchmarkSceneConfig config;
        config.kind = kind;
        config.seed = 1234;
        config.chunk_radius = 0;
        auto scene = BenchmarkScene::create(config);
        assert(scene);
        assert(scene.value()->world().chunks().chunk_count() == 1);
        assert(scene.value()->palette().size() == 5);
        assert(scene.value()->camera().view_projection.is_finite());
        assert(parse_benchmark_scene(benchmark_scene_name(kind)) == kind);
    }
    assert(!parse_benchmark_scene("not-a-scene"));
}

void test_active_water_scene_exposes_exact_stress_workload() {
    using namespace heartstead;
    renderer::benchmark::BenchmarkSceneConfig config;
    config.kind = renderer::benchmark::BenchmarkSceneKind::active_water;
    config.chunk_radius = 0;
    auto scene = renderer::benchmark::BenchmarkScene::create(config);
    assert(scene);
    auto fluids = world::ChunkFluidSystem::create(scene.value()->palette());
    assert(fluids);
    scene.value()->activate_fluid_work(*fluids.value());
    assert(fluids.value()->update(scene.value()->world().chunks(),
                                  scene.value()->world().dirty_regions(),
                                  scene.value()->palette(), 3));
    assert(fluids.value()->stats().processed_cells_this_update == 32'768);
    assert(!fluids.value()->stats().budget_exhausted);
    assert(fluids.value()->stats().active_cell_count == 0);
}

void test_scene_content_is_reproducible() {
    using namespace heartstead;
    renderer::benchmark::BenchmarkSceneConfig config;
    config.kind = renderer::benchmark::BenchmarkSceneKind::dense_caves;
    config.seed = 0x12345678;
    config.chunk_radius = 0;
    auto first = renderer::benchmark::BenchmarkScene::create(config);
    auto second = renderer::benchmark::BenchmarkScene::create(config);
    assert(first && second);
    auto different_seed_config = config;
    ++different_seed_config.seed;
    auto different_seed = renderer::benchmark::BenchmarkScene::create(different_seed_config);
    assert(different_seed);
    const auto* first_chunk = first.value()->world().chunks().records().front();
    const auto* second_chunk = second.value()->world().chunks().records().front();
    const auto* different_seed_chunk = different_seed.value()->world().chunks().records().front();
    std::size_t different_cell_count = 0;
    for (std::uint16_t z = 0; z < world::VoxelChunk::edge_length; ++z) {
        for (std::uint16_t y = 0; y < world::VoxelChunk::edge_length; ++y) {
            for (std::uint16_t x = 0; x < world::VoxelChunk::edge_length; ++x) {
                const auto first_cell = first_chunk->get({x, y, z});
                const auto second_cell = second_chunk->get({x, y, z});
                const auto other_cell = different_seed_chunk->get({x, y, z});
                assert(first_cell && second_cell && other_cell);
                assert(first_cell.value() == second_cell.value());
                different_cell_count += first_cell.value() != other_cell.value() ? 1U : 0U;
            }
        }
    }
    assert(different_cell_count > 0);
}

void test_dynamic_scene_schedules() {
    using namespace heartstead::renderer::benchmark;

    BenchmarkSceneConfig edit_config;
    edit_config.kind = BenchmarkSceneKind::rapid_voxel_edits;
    edit_config.chunk_radius = 0;
    auto edits = BenchmarkScene::create(edit_config);
    assert(edits);
    auto* edit_chunk = edits.value()->world().chunks().records().front();
    const auto old_revision = edit_chunk->content_revision();
    assert(edits.value()->advance(1));
    assert(edit_chunk->content_revision() > old_revision);

    BenchmarkSceneConfig churn_config;
    churn_config.kind = BenchmarkSceneKind::chunk_load_unload_churn;
    churn_config.chunk_radius = 0;
    auto churn = BenchmarkScene::create(churn_config);
    assert(churn);
    const auto old_identity = churn.value()->world().chunks().identities().front();
    assert(churn.value()->advance(0));
    const auto new_identity = churn.value()->world().chunks().identities().front();
    assert(new_identity.coordinate == old_identity.coordinate);
    assert(new_identity.load_generation > old_identity.load_generation);

    BenchmarkSceneConfig fly_config;
    fly_config.kind = BenchmarkSceneKind::large_coordinates;
    fly_config.chunk_radius = 0;
    auto flythrough = BenchmarkScene::create(fly_config);
    assert(flythrough);
    assert(flythrough.value()->advance(0));
    const auto first_flythrough_x = flythrough.value()->camera().floating_origin.block.x;
    assert(flythrough.value()->advance(3));
    const auto moved_flythrough_x = flythrough.value()->camera().floating_origin.block.x;
    assert(first_flythrough_x != moved_flythrough_x);
    assert(flythrough.value()->advance(99));
    assert(flythrough.value()->camera().floating_origin.block.x > 30'000'000'000LL);
    assert(std::abs(flythrough.value()->camera().floating_origin.block.x - 32'000'000'016LL) <=
           static_cast<std::int64_t>(heartstead::world::VoxelChunk::edge_length));
    assert(std::abs(flythrough.value()->camera().local_position.x) < 1.0F);

    BenchmarkSceneConfig resize_config;
    resize_config.kind = BenchmarkSceneKind::resize_minimize_stress;
    resize_config.chunk_radius = 0;
    auto resize = BenchmarkScene::create(resize_config);
    assert(resize);
    const auto minimized = resize.value()->advance(0);
    assert(minimized && minimized.value().skip_render);
    assert(minimized.value().requested_extent.has_value());
    assert(!minimized.value().requested_extent->is_valid());
    const auto restored = resize.value()->advance(1);
    assert(restored && !restored.value().skip_render);
    assert(restored.value().requested_extent.has_value());
    assert(restored.value().requested_extent->width == 800);
    assert(restored.value().requested_extent->height == 600);
}

} // namespace

int main() {
    test_benchmark_statistics();
    test_low_fps_windows_are_distinct();
    test_all_deterministic_scenes_construct();
    test_scene_content_is_reproducible();
    test_dynamic_scene_schedules();
    test_active_water_scene_exposes_exact_stress_workload();
    return 0;
}
