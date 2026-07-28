#pragma once

#include <cstdint>
#include <string>

namespace heartstead::renderer {

struct RendererStats {
    std::uint64_t frame_index = 0;
    std::uint64_t submission_serial = 0;
    std::uint64_t completed_submission_serial = 0;
    std::uint64_t gpu_timing_frame_index = 0;
    bool gpu_timing_valid = false;
    std::uint32_t gpu_timing_latency_frames = 0;
    std::uint64_t gpu_upload_submission_serial = 0;
    bool gpu_upload_timing_valid = false;

    double cpu_frame_ms = 0.0;
    double gpu_frame_ms = 0.0;
    double render_extraction_ms = 0.0;
    double chunk_synchronization_ms = 0.0;
    double culling_ms = 0.0;
    double draw_list_ms = 0.0;
    double command_build_ms = 0.0;
    double command_recording_ms = 0.0;
    double chunk_snapshot_ms = 0.0;
    double meshing_ms = 0.0;
    double upload_preparation_ms = 0.0;
    double upload_ms = 0.0;
    double gpu_wait_ms = 0.0;
    double voxel_relight_solve_ms = 0.0;
    double voxel_relight_apply_ms = 0.0;

    double gpu_opaque_terrain_ms = 0.0;
    double gpu_alpha_tested_terrain_ms = 0.0;
    double gpu_transparent_terrain_ms = 0.0;
    double gpu_upload_ms = 0.0;
    double gpu_transfer_ms = 0.0;
    double gpu_final_copy_ms = 0.0;

    std::uint32_t loaded_chunks = 0;
    std::uint32_t mesh_pending_chunks = 0;
    std::uint32_t upload_pending_chunks = 0;
    std::uint32_t resident_chunks = 0;
    std::uint32_t visible_chunks = 0;
    std::uint32_t culled_chunks = 0;
    std::uint32_t drawn_chunks = 0;
    std::uint32_t residency_suppressed_chunks = 0;
    std::uint32_t draw_calls = 0;
    std::uint32_t opaque_terrain_draws = 0;
    std::uint32_t alpha_tested_terrain_draws = 0;
    std::uint32_t transparent_terrain_draws = 0;
    std::uint32_t pipeline_switches = 0;
    std::uint32_t resident_textures = 0;
    std::uint32_t runtime_materials = 0;
    std::uint32_t resident_pipelines = 0;
    std::uint32_t retained_objects = 0;
    std::uint32_t visible_objects = 0;
    std::uint32_t culled_objects = 0;
    std::uint32_t instance_batches = 0;
    std::uint32_t submitted_instances = 0;
    std::uint32_t instance_draw_calls = 0;
    std::uint32_t dropped_instances = 0;
    std::uint32_t resident_static_meshes = 0;
    std::uint32_t debug_lines = 0;
    std::uint32_t debug_draw_calls = 0;
    std::uint32_t debug_labels = 0;
    std::uint32_t ui_draw_calls = 0;
    std::uint32_t ui_clipped_draw_calls = 0;
    std::uint32_t ui_vertices = 0;
    std::uint32_t ui_glyphs = 0;
    std::uint32_t voxel_relight_changed_chunks = 0;

    std::uint64_t vertices = 0;
    std::uint64_t triangles = 0;
    std::uint64_t resident_texture_bytes = 0;
    std::uint64_t resident_mesh_bytes = 0;
    std::uint64_t resident_static_mesh_bytes = 0;
    std::uint64_t uploaded_instance_bytes = 0;
    std::uint64_t debug_overflow = 0;
    std::uint64_t debug_uploaded_bytes = 0;
    std::uint64_t ui_uploaded_bytes = 0;
    std::uint64_t ui_overflow = 0;
    std::uint64_t gpu_terrain_budget_bytes = 0;
    std::uint64_t distance_evicted_meshes = 0;
    std::uint64_t memory_pressure_evicted_meshes = 0;
    std::uint64_t gpu_arena_capacity_bytes = 0;
    std::uint64_t gpu_arena_used_bytes = 0;
    std::uint64_t gpu_arena_free_bytes = 0;
    double gpu_arena_fragmentation = 0.0;
    std::uint64_t pending_upload_bytes = 0;
    std::uint64_t uploaded_bytes_this_frame = 0;
    std::uint64_t voxel_relight_backlog_cells = 0;
    std::uint64_t voxel_relight_visited_cells = 0;
    std::uint64_t voxel_relight_stale_results = 0;
    std::uint64_t voxel_relight_apply_budget_overruns = 0;
};

[[nodiscard]] std::string format_renderer_stats(const RendererStats& stats);

} // namespace heartstead::renderer
