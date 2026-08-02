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
           << " relight_failed=" << stats.voxel_relight_failed_results
           << " relight_budget_overruns=" << stats.voxel_relight_apply_budget_overruns
           << " fluid=" << stats.voxel_fluid_snapshot_ms << '/' << stats.voxel_fluid_simulation_ms
           << '/' << stats.voxel_fluid_apply_ms << "ms"
           << " fluid_cells=" << stats.voxel_fluid_processed_cells << '/'
           << stats.voxel_fluid_active_cells
           << " fluid_changed=" << stats.voxel_fluid_changed_chunks
           << " fluid_budget_exhaustions=" << stats.voxel_fluid_budget_exhaustions
           << " fluid_apply_budget_overruns=" << stats.voxel_fluid_apply_budget_overruns
           << " particles=" << stats.particle_active << '/' << stats.particle_emitters
           << " particle_cpu=" << stats.particle_update_ms << '/' << stats.particle_presentation_ms
           << "ms" << " particle_groups=" << stats.particle_material_groups
           << " particle_dropped=" << stats.particle_dropped << " chunks=" << stats.visible_chunks
           << '/' << stats.resident_chunks << '/' << stats.loaded_chunks
           << " far=" << stats.far_terrain_visible_patches << '/'
           << stats.far_terrain_resident_patches << '/'
           << stats.far_terrain_planned_patches << " far_pending="
           << stats.far_terrain_pending_patches << " far_bytes="
           << stats.far_terrain_resident_bytes << " far_upload="
           << stats.far_terrain_uploaded_bytes << " far_evicted="
           << stats.far_terrain_evicted_patches
           << " chunk_queue=" << stats.mesh_pending_chunks << '/' << stats.upload_pending_chunks
           << " edit_visible=" << stats.edit_to_visible_latest_ms << '/'
           << stats.edit_to_visible_recent_p95_ms << '/' << stats.edit_to_visible_session_max_ms
           << "ms edit_samples=" << stats.edit_to_visible_completed << '/'
           << stats.edit_to_visible_total_completed << '/'
           << stats.edit_to_visible_recent_samples
           << " edit_pending=" << stats.edit_to_visible_pending
           << " edit_coalesced=" << stats.edit_to_visible_total_coalesced
           << " edit_abandoned=" << stats.edit_to_visible_total_abandoned
           << " chunk_failed=" << stats.mesh_failures << '/' << stats.upload_failures
           << " chunk_stale=" << stats.stale_mesh_results << " mesh_work="
           << stats.mesh_total_completed_jobs << '/' << stats.mesh_total_built << '/'
           << stats.mesh_total_published << " mesh_amplification="
           << stats.mesh_builds_per_publication << " draws=" << stats.draw_calls << '/'
           << stats.indirect_draw_calls << '['
           << stats.opaque_terrain_draws << '/' << stats.alpha_tested_terrain_draws << '/'
           << stats.transparent_terrain_draws << ']' << " pipelines=" << stats.pipeline_switches
           << " textures=" << stats.resident_textures << '/' << stats.resident_texture_bytes
           << " materials=" << stats.runtime_materials << " triangles=" << stats.triangles
           << " resident_bytes=" << stats.resident_mesh_bytes << '/'
           << stats.gpu_terrain_budget_bytes << " objects=" << stats.visible_objects << '/'
           << stats.retained_objects << " visibility=" << stats.visibility_hierarchy_nodes << '/'
           << stats.visibility_nodes_tested << '/' << stats.visibility_nodes_culled
           << " instances=" << stats.submitted_instances << '/'
           << stats.instance_draw_calls << " skins=" << stats.submitted_skin_palettes << '/'
           << stats.submitted_skin_matrices << " skin_bytes=" << stats.uploaded_skin_matrix_bytes
           << " static_meshes=" << stats.resident_static_meshes << '/'
           << stats.resident_static_mesh_bytes << " device_memory="
           << stats.device_local_memory_usage_bytes << '/'
           << stats.device_local_memory_budget_bytes << " streaming="
           << stats.streaming_resident_resources << '/'
           << stats.streaming_tracked_resources << '/' << stats.streaming_pending_resources
           << " streaming_bytes=" << stats.streaming_resident_bytes << '/'
           << stats.streaming_pending_bytes << " streaming_upload="
           << stats.streaming_uploaded_bytes << " streaming_evicted="
           << stats.streaming_evicted_resources << " debug=" << stats.debug_lines << '/'
           << stats.debug_draw_calls << '/' << stats.debug_labels
           << " debug_overflow=" << stats.debug_overflow << " ui=" << stats.ui_vertices << '/'
           << stats.ui_draw_calls << '/' << stats.ui_clipped_draw_calls
           << " glyphs=" << stats.ui_glyphs << " widgets=" << stats.ui_widgets
           << " ui_cpu=" << stats.ui_layout_ms << '/' << stats.ui_paint_ms << "ms"
           << " ui_overflow=" << stats.ui_overflow
           << " graph=" << stats.render_graph_passes << '/' << stats.render_graph_resources
           << '/' << stats.render_graph_transient_resources << '/'
           << stats.render_graph_persistent_resources << '/'
           << stats.render_graph_history_resources
           << " lights=" << stats.local_lights << '/' << stats.dropped_local_lights
           << " shadowed=" << stats.shadowed_local_lights
           << " shaders=" << stats.resident_shader_programs << '/'
           << stats.resident_shader_modules << " shader_errors=" << stats.shader_errors
           << " pipeline_errors=" << stats.pipeline_errors
           << " layouts=" << stats.resident_pipeline_layouts
           << " descriptors=" << stats.descriptor_bindings
           << " samplers=" << stats.resident_samplers
           << " suppressed=" << stats.residency_suppressed_chunks
           << " evicted=" << stats.distance_evicted_meshes << '/'
           << stats.memory_pressure_evicted_meshes << " arena=" << stats.gpu_arena_used_bytes << '/'
           << stats.gpu_arena_capacity_bytes << " arena_frag=" << stats.gpu_arena_fragmentation;
    return stream.str();
}

} // namespace heartstead::renderer
