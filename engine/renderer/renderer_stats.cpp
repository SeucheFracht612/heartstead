#include "engine/renderer/renderer_stats.hpp"

#include <iomanip>
#include <sstream>

namespace heartstead::renderer {

std::string format_renderer_stats(const RendererStats& stats) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << "frame=" << stats.frame_index
           << " submit=" << stats.submission_serial << '/' << stats.completed_submission_serial
           << " cpu=" << stats.cpu_frame_ms << "ms";
    if (stats.gpu_timing_valid) {
        stream << " gpu=" << stats.gpu_frame_ms << "ms"
               << " gpu_frame=" << stats.gpu_timing_frame_index
               << " phases=" << stats.gpu_opaque_terrain_ms << '/'
               << stats.gpu_alpha_tested_terrain_ms << '/' << stats.gpu_transparent_terrain_ms
               << "ms";
    } else {
        stream << " gpu=pending";
    }
    if (stats.gpu_upload_timing_valid) {
        stream << " gpu_upload=" << stats.gpu_upload_ms << "ms"
               << " upload_submit=" << stats.gpu_upload_submission_serial;
    }
    stream << " sync=" << stats.chunk_synchronization_ms << "ms" << " mesh=" << stats.meshing_ms
           << "ms" << " upload=" << stats.upload_ms << "ms" << " cull=" << stats.culling_ms << "ms"
           << " record=" << stats.command_recording_ms << "ms" << " wait=" << stats.gpu_wait_ms
           << "ms" << " relight=" << stats.voxel_relight_solve_ms << '/'
           << stats.voxel_relight_apply_ms << "ms"
           << " relight_cells=" << stats.voxel_relight_visited_cells << '/'
           << stats.voxel_relight_backlog_cells
           << " relight_changed=" << stats.voxel_relight_changed_chunks
           << " relight_stale=" << stats.voxel_relight_stale_results
           << " relight_budget_overruns=" << stats.voxel_relight_apply_budget_overruns
           << " fluid=" << stats.voxel_fluid_snapshot_ms << '/' << stats.voxel_fluid_simulation_ms
           << '/' << stats.voxel_fluid_apply_ms << "ms"
           << " fluid_cells=" << stats.voxel_fluid_processed_cells << '/'
           << stats.voxel_fluid_active_cells
           << " fluid_changed=" << stats.voxel_fluid_changed_chunks
           << " fluid_budget_exhaustions=" << stats.voxel_fluid_budget_exhaustions
           << " fluid_apply_budget_overruns=" << stats.voxel_fluid_apply_budget_overruns
           << " particles=" << stats.particle_active << '/' << stats.particle_emitters
           << " particle_cpu=" << stats.particle_update_ms << '/'
           << stats.particle_presentation_ms << "ms"
           << " particle_groups=" << stats.particle_material_groups
           << " particle_dropped=" << stats.particle_dropped
           << " chunks=" << stats.visible_chunks << '/' << stats.resident_chunks << '/'
           << stats.loaded_chunks << " draws=" << stats.draw_calls << '['
           << stats.opaque_terrain_draws << '/' << stats.alpha_tested_terrain_draws << '/'
           << stats.transparent_terrain_draws << ']' << " pipelines=" << stats.pipeline_switches
           << " textures=" << stats.resident_textures << '/' << stats.resident_texture_bytes
           << " materials=" << stats.runtime_materials << " triangles=" << stats.triangles
           << " resident_bytes=" << stats.resident_mesh_bytes << '/'
           << stats.gpu_terrain_budget_bytes << " objects=" << stats.visible_objects << '/'
           << stats.retained_objects << " instances=" << stats.submitted_instances << '/'
           << stats.instance_draw_calls << " skins=" << stats.submitted_skin_palettes << '/'
           << stats.submitted_skin_matrices << " skin_bytes=" << stats.uploaded_skin_matrix_bytes
           << " static_meshes=" << stats.resident_static_meshes << '/'
           << stats.resident_static_mesh_bytes << " debug=" << stats.debug_lines << '/'
           << stats.debug_draw_calls << '/' << stats.debug_labels
           << " debug_overflow=" << stats.debug_overflow << " ui=" << stats.ui_vertices << '/'
           << stats.ui_draw_calls << '/' << stats.ui_clipped_draw_calls
           << " glyphs=" << stats.ui_glyphs << " widgets=" << stats.ui_widgets
           << " ui_cpu=" << stats.ui_layout_ms << '/' << stats.ui_paint_ms << "ms"
           << " ui_overflow=" << stats.ui_overflow
           << " suppressed=" << stats.residency_suppressed_chunks
           << " evicted=" << stats.distance_evicted_meshes << '/'
           << stats.memory_pressure_evicted_meshes << " arena=" << stats.gpu_arena_used_bytes << '/'
           << stats.gpu_arena_capacity_bytes << " arena_frag=" << stats.gpu_arena_fragmentation;
    return stream.str();
}

} // namespace heartstead::renderer
