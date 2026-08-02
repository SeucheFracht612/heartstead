function(heartstead_configure_test_target target_name)
  heartstead_configure_target(${target_name})
  # The tests deliberately use assert() as their lightweight check primitive. Keep those checks
  # active in Release/RelWithDebInfo configurations, where toolchains otherwise define NDEBUG.
  if(MSVC)
    target_compile_options(${target_name} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${target_name} PRIVATE -UNDEBUG)
  endif()
endfunction()

function(heartstead_register_unit_test target_name)
  add_test(NAME ${target_name} COMMAND ${target_name})
  # A deadlock in a foundation test must fail CI instead of occupying a worker indefinitely.
  # Keep this above the smoke-test timeout because sanitizer and contention builds are slower.
  set_tests_properties(${target_name} PROPERTIES TIMEOUT 120)
endfunction()

add_executable(heartstead_engine_tests engine_tests.cpp)
target_link_libraries(heartstead_engine_tests PRIVATE Heartstead::Engine KTX::ktx)
heartstead_configure_test_target(heartstead_engine_tests)

heartstead_register_unit_test(heartstead_engine_tests)

add_executable(heartstead_job_system_tests job_system_tests.cpp)
target_link_libraries(heartstead_job_system_tests PRIVATE Heartstead::Engine)
heartstead_configure_test_target(heartstead_job_system_tests)
heartstead_register_unit_test(heartstead_job_system_tests)

add_executable(heartstead_chunk_stage_ledger_tests chunk_stage_ledger_tests.cpp)
target_link_libraries(heartstead_chunk_stage_ledger_tests PRIVATE Heartstead::Engine)
heartstead_configure_test_target(heartstead_chunk_stage_ledger_tests)
heartstead_register_unit_test(heartstead_chunk_stage_ledger_tests)

add_executable(heartstead_renderer_frame_graph_tests renderer_frame_graph_tests.cpp)
target_link_libraries(heartstead_renderer_frame_graph_tests PRIVATE Heartstead::Engine)
heartstead_configure_test_target(heartstead_renderer_frame_graph_tests)

heartstead_register_unit_test(heartstead_renderer_frame_graph_tests)

add_executable(heartstead_renderer_tone_mapping_tests renderer_tone_mapping_tests.cpp)
target_link_libraries(heartstead_renderer_tone_mapping_tests PRIVATE Heartstead::Engine)
target_compile_definitions(
  heartstead_renderer_tone_mapping_tests
  PRIVATE HEARTSTEAD_BUILTIN_SHADER_DIR="${PROJECT_SOURCE_DIR}/engine/renderer/shaders/builtin"
)
heartstead_configure_test_target(heartstead_renderer_tone_mapping_tests)

heartstead_register_unit_test(heartstead_renderer_tone_mapping_tests)

add_executable(heartstead_renderer_compute_tests renderer_compute_tests.cpp)
target_link_libraries(heartstead_renderer_compute_tests PRIVATE Heartstead::Engine)
target_compile_definitions(
  heartstead_renderer_compute_tests
  PRIVATE HEARTSTEAD_BUILTIN_SHADER_DIR="${PROJECT_SOURCE_DIR}/engine/renderer/shaders/builtin"
)
heartstead_configure_test_target(heartstead_renderer_compute_tests)

heartstead_register_unit_test(heartstead_renderer_compute_tests)

add_executable(heartstead_renderer_indirect_gpu_tests renderer_indirect_gpu_tests.cpp)
target_link_libraries(heartstead_renderer_indirect_gpu_tests PRIVATE Heartstead::Engine)
target_compile_definitions(
  heartstead_renderer_indirect_gpu_tests
  PRIVATE HEARTSTEAD_BUILTIN_SHADER_DIR="${PROJECT_SOURCE_DIR}/engine/renderer/shaders/builtin"
)
heartstead_configure_test_target(heartstead_renderer_indirect_gpu_tests)
heartstead_register_unit_test(heartstead_renderer_indirect_gpu_tests)

add_executable(heartstead_renderer_lighting_tests renderer_lighting_tests.cpp)
target_link_libraries(heartstead_renderer_lighting_tests PRIVATE Heartstead::Engine)
heartstead_configure_test_target(heartstead_renderer_lighting_tests)
heartstead_register_unit_test(heartstead_renderer_lighting_tests)

add_executable(heartstead_vegetation_renderer_tests vegetation_renderer_tests.cpp)
target_link_libraries(heartstead_vegetation_renderer_tests PRIVATE Heartstead::Engine)
target_compile_definitions(
  heartstead_vegetation_renderer_tests
  PRIVATE
    HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    HEARTSTEAD_TEST_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
)
if(TARGET heartstead_dev_game_model_assets)
  add_dependencies(heartstead_vegetation_renderer_tests heartstead_dev_game_model_assets)
endif()
heartstead_configure_test_target(heartstead_vegetation_renderer_tests)
heartstead_register_unit_test(heartstead_vegetation_renderer_tests)

add_executable(heartstead_large_water_renderer_tests large_water_renderer_tests.cpp)
target_link_libraries(heartstead_large_water_renderer_tests PRIVATE Heartstead::Engine)
heartstead_configure_test_target(heartstead_large_water_renderer_tests)
heartstead_register_unit_test(heartstead_large_water_renderer_tests)

add_executable(heartstead_environment_effects_tests environment_effects_tests.cpp)
target_link_libraries(heartstead_environment_effects_tests PRIVATE Heartstead::Engine)
heartstead_configure_test_target(heartstead_environment_effects_tests)
heartstead_register_unit_test(heartstead_environment_effects_tests)

add_executable(heartstead_game_runtime_tests game_runtime_tests.cpp)
target_link_libraries(heartstead_game_runtime_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_game_runtime_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_game_runtime_tests)

heartstead_register_unit_test(heartstead_game_runtime_tests)

add_executable(heartstead_game_application_tests game_application_tests.cpp)
target_link_libraries(heartstead_game_application_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_game_application_tests)
heartstead_register_unit_test(heartstead_game_application_tests)

add_executable(heartstead_application_state_tests application_state_tests.cpp)
target_link_libraries(heartstead_application_state_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_application_state_tests)
heartstead_register_unit_test(heartstead_application_state_tests)

add_executable(heartstead_runtime_session_lifecycle_tests runtime_session_lifecycle_tests.cpp)
target_link_libraries(heartstead_runtime_session_lifecycle_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_runtime_session_lifecycle_tests
  PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_runtime_session_lifecycle_tests)
heartstead_register_unit_test(heartstead_runtime_session_lifecycle_tests)

add_executable(heartstead_game_shell_lifecycle_stress_tests
               game_shell_lifecycle_stress_tests.cpp)
target_link_libraries(heartstead_game_shell_lifecycle_stress_tests
                      PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_game_shell_lifecycle_stress_tests
  PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_game_shell_lifecycle_stress_tests)
heartstead_register_unit_test(heartstead_game_shell_lifecycle_stress_tests)
set_tests_properties(heartstead_game_shell_lifecycle_stress_tests
                     PROPERTIES TIMEOUT 240 LABELS "lifecycle;stress")

add_executable(heartstead_developer_world_tests developer_world_tests.cpp)
target_link_libraries(heartstead_developer_world_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_developer_world_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_developer_world_tests)
heartstead_register_unit_test(heartstead_developer_world_tests)

add_executable(heartstead_front_end_tests front_end_tests.cpp)
target_link_libraries(heartstead_front_end_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_front_end_tests)
heartstead_register_unit_test(heartstead_front_end_tests)

if(TARGET heartstead_dev_game)
  add_test(
    NAME heartstead_dev_game_save_smoke
    COMMAND
      ${CMAKE_COMMAND}
      -DDEV_GAME=$<TARGET_FILE:heartstead_dev_game>
      -DSAVE_ROOT=${CMAKE_CURRENT_BINARY_DIR}/dev_game_save_smoke
      -P ${CMAKE_CURRENT_SOURCE_DIR}/dev_game_save_smoke.cmake
  )
  set_tests_properties(heartstead_dev_game_save_smoke PROPERTIES TIMEOUT 120 LABELS smoke)
endif()

if(TARGET heartstead)
  file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/heartstead_blocked_data_root "not-a-directory")
  add_test(
    NAME heartstead_degraded_persistence_smoke
    COMMAND
      ${CMAKE_COMMAND} -E env
      HEARTSTEAD_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/heartstead_blocked_data_root
      $<TARGET_FILE:heartstead> --frames 5
  )
  set_tests_properties(
    heartstead_degraded_persistence_smoke
    PROPERTIES TIMEOUT 60 LABELS "smoke;recovery"
               PASS_REGULAR_EXPRESSION "state=MainMenu"
  )
  file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/heartstead_invalid_content_smoke)
  add_test(
    NAME heartstead_content_recovery_smoke
    COMMAND
      ${CMAKE_COMMAND} -E env
      HEARTSTEAD_CONTENT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/heartstead_invalid_content_smoke
      HEARTSTEAD_DATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/heartstead_content_recovery_data
      $<TARGET_FILE:heartstead> --frames 2
  )
  set_tests_properties(
    heartstead_content_recovery_smoke
    PROPERTIES TIMEOUT 60 LABELS "smoke;recovery"
               PASS_REGULAR_EXPRESSION "startup recovery: diagnostics="
  )
  add_test(NAME heartstead_menu_headless_smoke COMMAND heartstead --frames 5)
  set_tests_properties(heartstead_menu_headless_smoke PROPERTIES TIMEOUT 60 LABELS smoke)
  add_test(
    NAME heartstead_loading_shutdown_smoke
    COMMAND heartstead --scenario base:scenarios/foundation_slice --frames 5
  )
  set_tests_properties(heartstead_loading_shutdown_smoke
                       PROPERTIES TIMEOUT 60 LABELS "smoke;lifecycle")
  add_test(
    NAME heartstead_active_session_shutdown_smoke
    COMMAND heartstead --scenario base:scenarios/foundation_slice --frames 10000
  )
  set_tests_properties(
    heartstead_active_session_shutdown_smoke
    PROPERTIES TIMEOUT 60 LABELS "smoke;lifecycle"
               PASS_REGULAR_EXPRESSION "state=InGame"
  )
  add_test(
    NAME heartstead_production_lifecycle_smoke
    COMMAND
      ${CMAKE_COMMAND}
      -DHEARTSTEAD=$<TARGET_FILE:heartstead>
      -DDATA_ROOT=${CMAKE_CURRENT_BINARY_DIR}/heartstead_production_lifecycle_smoke
      -P ${CMAKE_CURRENT_SOURCE_DIR}/heartstead_production_lifecycle_smoke.cmake
  )
  set_tests_properties(heartstead_production_lifecycle_smoke
                       PROPERTIES TIMEOUT 120 LABELS "smoke;lifecycle;persistence")
endif()

add_executable(heartstead_client_audio_presentation_tests client_audio_presentation_tests.cpp)
target_link_libraries(heartstead_client_audio_presentation_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_client_audio_presentation_tests)
heartstead_register_unit_test(heartstead_client_audio_presentation_tests)

add_executable(heartstead_runtime_spine_tests runtime_spine_tests.cpp)
target_link_libraries(heartstead_runtime_spine_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_runtime_spine_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_runtime_spine_tests)
heartstead_register_unit_test(heartstead_runtime_spine_tests)

add_executable(heartstead_presentation_world_tests presentation_world_tests.cpp)
target_link_libraries(heartstead_presentation_world_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_presentation_world_tests)
heartstead_register_unit_test(heartstead_presentation_world_tests)

add_executable(heartstead_animated_model_presentation_tests
               animated_model_presentation_tests.cpp)
target_link_libraries(heartstead_animated_model_presentation_tests
                      PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_animated_model_presentation_tests)
heartstead_register_unit_test(heartstead_animated_model_presentation_tests)

add_executable(heartstead_model_presentation_system_tests model_presentation_system_tests.cpp)
target_link_libraries(heartstead_model_presentation_system_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_model_presentation_system_tests
  PRIVATE
    HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    HEARTSTEAD_TEST_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
)
if(TARGET heartstead_dev_game_model_assets)
  add_dependencies(heartstead_model_presentation_system_tests
                   heartstead_dev_game_model_assets)
endif()
heartstead_configure_test_target(heartstead_model_presentation_system_tests)
heartstead_register_unit_test(heartstead_model_presentation_system_tests)

if(TARGET heartstead_asset_lab_runtime)
  add_executable(heartstead_asset_lab_tests asset_lab_tests.cpp)
  target_link_libraries(heartstead_asset_lab_tests PRIVATE heartstead_asset_lab_runtime)
  target_compile_definitions(
    heartstead_asset_lab_tests
    PRIVATE
      HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
      HEARTSTEAD_TEST_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
  )
  if(TARGET heartstead_dev_game_model_assets)
    add_dependencies(heartstead_asset_lab_tests heartstead_dev_game_model_assets)
  endif()
  heartstead_configure_test_target(heartstead_asset_lab_tests)
  heartstead_register_unit_test(heartstead_asset_lab_tests)
endif()

add_executable(heartstead_headless_session_tests headless_session_tests.cpp)
target_link_libraries(heartstead_headless_session_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_headless_session_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_headless_session_tests)
heartstead_register_unit_test(heartstead_headless_session_tests)

add_executable(heartstead_foundation_world_tests foundation_world_tests.cpp)
target_link_libraries(heartstead_foundation_world_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_foundation_world_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_foundation_world_tests)
heartstead_register_unit_test(heartstead_foundation_world_tests)

function(heartstead_add_foundation_test target_name source_file)
  add_executable(${target_name} ${source_file})
  target_link_libraries(${target_name} PRIVATE Heartstead::Engine)
  heartstead_configure_test_target(${target_name})
  heartstead_register_unit_test(${target_name})
endfunction()

heartstead_add_foundation_test(heartstead_coordinate_v02_tests coordinate_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_world_integrity_v02_tests world_integrity_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_replication_v02_tests replication_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_server_foundations_v02_tests
                               server_foundations_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_rich_blocks_v02_tests rich_blocks_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_region_storage_v02_tests region_storage_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_save_recovery_v02_tests save_recovery_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_save_journal_tests save_journal_tests.cpp)
heartstead_add_foundation_test(heartstead_save_scheduler_tests save_scheduler_tests.cpp)
heartstead_add_foundation_test(heartstead_chunk_load_scheduler_tests
                               chunk_load_scheduler_tests.cpp)
heartstead_add_foundation_test(heartstead_authoritative_server_v02_tests
                               authoritative_server_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_lazy_process_fire_v02_tests
                               lazy_process_fire_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_workpiece_v02_tests workpiece_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_assembly_v02_tests assembly_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_infrastructure_v02_tests infrastructure_v02_tests.cpp)
heartstead_add_foundation_test(heartstead_transport_integrity_tests transport_integrity_tests.cpp)
heartstead_add_foundation_test(heartstead_transport_codec_fuzz_tests
                               transport_codec_fuzz_tests.cpp)
heartstead_add_foundation_test(heartstead_scripting_runtime_tests
                               scripting_runtime_tests.cpp)
heartstead_add_foundation_test(heartstead_filesystem_tests filesystem_tests.cpp)
heartstead_add_foundation_test(heartstead_render_milestone_tests render_milestone_tests.cpp)
heartstead_add_foundation_test(heartstead_indirect_draw_tests indirect_draw_tests.cpp)
heartstead_add_foundation_test(heartstead_renderer_frontend_tests renderer_frontend_tests.cpp)
target_compile_definitions(
  heartstead_renderer_frontend_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_add_foundation_test(heartstead_chunk_mesh_scheduler_tests
                               chunk_mesh_scheduler_tests.cpp)
heartstead_add_foundation_test(heartstead_chunk_collision_tests chunk_collision_tests.cpp)
heartstead_add_foundation_test(heartstead_physics_character_tests physics_character_tests.cpp)
heartstead_add_foundation_test(heartstead_physical_resource_physics_tests
                               physical_resource_physics_tests.cpp)
heartstead_add_foundation_test(heartstead_audio_system_tests audio_system_tests.cpp)
target_compile_definitions(
  heartstead_audio_system_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_add_foundation_test(heartstead_model_asset_tests model_asset_tests.cpp)
target_compile_definitions(
  heartstead_model_asset_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_add_foundation_test(heartstead_model_compression_tests
                               model_compression_tests.cpp)
target_link_libraries(
  heartstead_model_compression_tests
  PRIVATE draco::draco KTX::ktx meshoptimizer::meshoptimizer
)
heartstead_add_foundation_test(heartstead_texture_asset_tests texture_asset_tests.cpp)
heartstead_add_foundation_test(heartstead_skeletal_animation_tests skeletal_animation_tests.cpp)
heartstead_add_foundation_test(heartstead_animation_budget_tests animation_budget_tests.cpp)
heartstead_add_foundation_test(heartstead_entity_visual_v2_tests entity_visual_v2_tests.cpp)
heartstead_add_foundation_test(heartstead_equipment_attachment_tests equipment_attachment_tests.cpp)
heartstead_add_foundation_test(heartstead_equipment_loadout_tests equipment_loadout_tests.cpp)
heartstead_add_foundation_test(heartstead_entity_motion_visual_state_tests entity_motion_visual_state_tests.cpp)
heartstead_add_foundation_test(heartstead_animation_interaction_tests animation_interaction_tests.cpp)
heartstead_add_foundation_test(heartstead_voxel_light_tests voxel_light_tests.cpp)
heartstead_add_foundation_test(heartstead_voxel_fluid_tests voxel_fluid_tests.cpp)
heartstead_add_foundation_test(heartstead_voxel_mask_tests voxel_mask_tests.cpp)
heartstead_add_foundation_test(heartstead_voxel_storage_experiment_tests
                               voxel_storage_experiment_tests.cpp)
heartstead_add_foundation_test(heartstead_voxel_storage_benchmark_tests
                               voxel_storage_benchmark_tests.cpp)
heartstead_add_foundation_test(heartstead_voxel_meshing_benchmark_tests
                               voxel_meshing_benchmark_tests.cpp)
heartstead_add_foundation_test(heartstead_chunk_light_system_tests
                               chunk_light_system_tests.cpp)
heartstead_add_foundation_test(heartstead_day_night_environment_tests
                               day_night_environment_tests.cpp)
heartstead_add_foundation_test(heartstead_environment_profile_tests
                               environment_profile_tests.cpp)
heartstead_add_foundation_test(heartstead_gpu_buffer_arena_tests gpu_buffer_arena_tests.cpp)
heartstead_add_foundation_test(heartstead_streaming_residency_tests
                               streaming_residency_tests.cpp)
heartstead_add_foundation_test(heartstead_renderer_streaming_ownership_tests
                               renderer_streaming_ownership_tests.cpp)
heartstead_add_foundation_test(heartstead_renderer_benchmark_tests renderer_benchmark_tests.cpp)
heartstead_add_foundation_test(heartstead_benchmark_coverage_tests benchmark_coverage_tests.cpp)
heartstead_add_foundation_test(heartstead_render_runtime_asset_tests
                               render_runtime_asset_tests.cpp)
heartstead_add_foundation_test(heartstead_greedy_chunk_mesher_tests
                               greedy_chunk_mesher_tests.cpp)
heartstead_add_foundation_test(heartstead_far_terrain_clipmap_tests
                               far_terrain_clipmap_tests.cpp)
heartstead_add_foundation_test(heartstead_far_terrain_renderer_tests
                               far_terrain_renderer_tests.cpp)
heartstead_add_foundation_test(heartstead_render_scene_tests render_scene_tests.cpp)
heartstead_add_foundation_test(heartstead_visibility_hierarchy_tests
                               visibility_hierarchy_tests.cpp)
heartstead_add_foundation_test(heartstead_hierarchical_depth_occlusion_tests
                               hierarchical_depth_occlusion_tests.cpp)
heartstead_add_foundation_test(heartstead_mesh_manager_tests mesh_manager_tests.cpp)
heartstead_add_foundation_test(heartstead_debug_renderer_tests debug_renderer_tests.cpp)
heartstead_add_foundation_test(heartstead_ui_renderer_tests ui_renderer_tests.cpp)
target_compile_definitions(
  heartstead_ui_renderer_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_add_foundation_test(heartstead_ui_font_tests ui_font_tests.cpp)
target_compile_definitions(
  heartstead_ui_font_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_add_foundation_test(heartstead_renderer_quality_tests renderer_quality_tests.cpp)
heartstead_add_foundation_test(heartstead_visual_regression_tests visual_regression_tests.cpp)
heartstead_add_foundation_test(heartstead_widget_tree_tests widget_tree_tests.cpp)
target_compile_definitions(
  heartstead_widget_tree_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_add_foundation_test(heartstead_map_view_tests map_view_tests.cpp)
heartstead_add_foundation_test(heartstead_asset_safety_tests asset_safety_tests.cpp)
heartstead_add_foundation_test(heartstead_platform_input_edge_tests platform_input_edge_tests.cpp)
heartstead_add_foundation_test(heartstead_render_device_state_tests render_device_state_tests.cpp)
heartstead_add_foundation_test(heartstead_simulation_runtime_tests simulation_runtime_tests.cpp)
heartstead_add_foundation_test(heartstead_entity_world_tests entity_world_tests.cpp)
heartstead_add_foundation_test(heartstead_entity_motion_snapshot_tests
                               entity_motion_snapshot_tests.cpp)
heartstead_add_foundation_test(heartstead_particle_system_tests particle_system_tests.cpp)
heartstead_add_foundation_test(heartstead_movement_controller_tests movement_controller_tests.cpp)
heartstead_add_foundation_test(heartstead_file_io_tests file_io_tests.cpp)
heartstead_add_foundation_test(heartstead_process_entry_tests process_entry_tests.cpp)

add_executable(heartstead_gameplay_input_tests gameplay_input_tests.cpp)
target_link_libraries(heartstead_gameplay_input_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_gameplay_input_tests)
heartstead_register_unit_test(heartstead_gameplay_input_tests)

add_executable(heartstead_game_ui_tests game_ui_tests.cpp)
target_link_libraries(heartstead_game_ui_tests PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_game_ui_tests PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_game_ui_tests)
heartstead_register_unit_test(heartstead_game_ui_tests)

add_executable(heartstead_particle_presentation_tests particle_presentation_tests.cpp)
target_link_libraries(heartstead_particle_presentation_tests PRIVATE Heartstead::GameRuntime)
heartstead_configure_test_target(heartstead_particle_presentation_tests)
heartstead_register_unit_test(heartstead_particle_presentation_tests)

add_executable(heartstead_voxel_interaction_presentation_tests
               voxel_interaction_presentation_tests.cpp)
target_link_libraries(heartstead_voxel_interaction_presentation_tests
                      PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_voxel_interaction_presentation_tests
  PRIVATE HEARTSTEAD_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_test_target(heartstead_voxel_interaction_presentation_tests)
heartstead_register_unit_test(heartstead_voxel_interaction_presentation_tests)

function(heartstead_add_smoke_test test_name target_name)
  if(TARGET ${target_name})
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}> ${ARGN})
    set_tests_properties(${test_name} PROPERTIES LABELS smoke TIMEOUT 30)
  endif()
endfunction()

heartstead_add_smoke_test(smoke.platform heartstead_platform_smoke)
heartstead_add_smoke_test(smoke.renderer heartstead_renderer_smoke)
heartstead_add_smoke_test(
  smoke.render_benchmark
  heartstead_render_benchmark
  --scene
  flat
  --radius
  0
  --warmup
  1
  --frames
  3
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_render_benchmark.json
)
heartstead_add_smoke_test(
  smoke.render_benchmark_relight
  heartstead_render_benchmark
  --scene
  rapid-edits
  --radius
  0
  --warmup
  1
  --frames
  3
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_render_benchmark_relight.json
)
heartstead_add_smoke_test(
  smoke.render_benchmark_fluids
  heartstead_render_benchmark
  --scene
  active-water
  --radius
  0
  --warmup
  1
  --frames
  3
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_render_benchmark_fluids.json
)
heartstead_add_smoke_test(
  smoke.render_benchmark_particles
  heartstead_render_benchmark
  --scene
  particles
  --radius
  0
  --warmup
  0
  --frames
  1
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_render_benchmark_particles.json
)
heartstead_add_smoke_test(
  smoke.render_benchmark_terrain_materials
  heartstead_render_benchmark
  --scene
  terrain-materials
  --radius
  0
  --warmup
  1
  --frames
  3
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_render_benchmark_terrain_materials.json
)
heartstead_add_smoke_test(
  smoke.render_benchmark_character_workshop
  heartstead_render_benchmark
  --scene
  character-workshop
  --radius
  0
  --warmup
  1
  --frames
  3
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_render_benchmark_character_workshop.json
)
# These stress scenes include deterministic chunk-light settlement before and after the
# measured frames. Debug builds can legitimately exceed the generic smoke-test budget even
# though the measured renderer workload completes promptly.
set_tests_properties(
  smoke.render_benchmark_particles
  smoke.render_benchmark_character_workshop
  PROPERTIES TIMEOUT 120
)
heartstead_add_smoke_test(
  smoke.ui_benchmark
  heartstead_ui_benchmark
  --widgets
  100
  --warmup
  1
  --frames
  3
  --output
  ${CMAKE_CURRENT_BINARY_DIR}/smoke_ui_benchmark.json
)
heartstead_add_smoke_test(smoke.audio_benchmark heartstead_audio_benchmark --voices 16 --warmup 2
                          --blocks 3)
heartstead_add_smoke_test(smoke.scripting_benchmark heartstead_scripting_benchmark --modules 4
                          --warmup 10 --calls 50
                          --output ${CMAKE_CURRENT_BINARY_DIR}/smoke_scripting_benchmark.json)
heartstead_add_smoke_test(smoke.dev_game heartstead_dev_game --frames 3)
heartstead_add_smoke_test(smoke.asset_lab heartstead_asset_lab --headless
                          --prefab base:visuals/player --preview visual-prefab)
heartstead_add_smoke_test(smoke.dedicated_server heartstead_dedicated_server
                          --bind 127.0.0.1:0 --ticks 3)
heartstead_add_smoke_test(smoke.physics heartstead_physics_sandbox)
heartstead_add_smoke_test(smoke.network heartstead_network_sandbox)
heartstead_add_smoke_test(smoke.scripting heartstead_scripting_sandbox)
heartstead_add_smoke_test(smoke.jobs heartstead_jobs_sandbox)
heartstead_add_smoke_test(smoke.math heartstead_math_sandbox)
heartstead_add_smoke_test(smoke.mod_sandbox heartstead_mod_sandbox)
heartstead_add_smoke_test(smoke.workpiece_sandbox heartstead_workpiece_sandbox)
heartstead_add_smoke_test(smoke.chunk_sandbox heartstead_chunk_sandbox)
heartstead_add_smoke_test(smoke.room_sandbox heartstead_room_sandbox)
heartstead_add_smoke_test(smoke.world_state_sandbox heartstead_world_state_sandbox)

heartstead_add_smoke_test(smoke.asset_cooker heartstead_asset_cooker ${PROJECT_SOURCE_DIR}
                          ${CMAKE_CURRENT_BINARY_DIR}/smoke_asset_manifest.txt development)
heartstead_add_smoke_test(smoke.asset_cooker_inspect heartstead_asset_cooker ${PROJECT_SOURCE_DIR}
                          ${CMAKE_CURRENT_BINARY_DIR}/smoke_asset_manifest_inspect.txt development
                          --inspect)
heartstead_add_smoke_test(smoke.mod_validator heartstead_mod_validator ${PROJECT_SOURCE_DIR})
heartstead_add_smoke_test(smoke.mod_validator_inspect heartstead_mod_validator ${PROJECT_SOURCE_DIR}
                          --inspect)
heartstead_add_smoke_test(smoke.prototype_inspector heartstead_prototype_inspector
                          ${PROJECT_SOURCE_DIR} base:items/raw_clay)
heartstead_add_smoke_test(smoke.replay_inspector heartstead_replay_inspector
                          ${PROJECT_SOURCE_DIR}/tests/fixtures/sample_command_replay.txt)
heartstead_add_smoke_test(smoke.save_inspector heartstead_save_inspector
                          ${PROJECT_SOURCE_DIR}/tests/fixtures/minimal_save_snapshot.txt
                          ${PROJECT_SOURCE_DIR})
heartstead_add_smoke_test(smoke.save_slot_inspector heartstead_save_inspector --slots
                          ${PROJECT_SOURCE_DIR}/tests/fixtures/save_slots)
heartstead_add_smoke_test(smoke.shader_compiler heartstead_shader_compiler ${PROJECT_SOURCE_DIR}
                          ${CMAKE_CURRENT_BINARY_DIR}/smoke_shader_manifest.txt development)
heartstead_add_smoke_test(smoke.shader_compiler_inspect heartstead_shader_compiler
                          ${PROJECT_SOURCE_DIR}
                          ${CMAKE_CURRENT_BINARY_DIR}/smoke_shader_manifest_inspect.txt
                          development --inspect)
heartstead_add_smoke_test(smoke.block_model_viewer heartstead_block_model_viewer
                          ${PROJECT_SOURCE_DIR})
heartstead_add_smoke_test(smoke.chunk_inspector heartstead_chunk_inspector --help)
heartstead_add_smoke_test(smoke.workpiece_inspector heartstead_workpiece_inspector
                          ${PROJECT_SOURCE_DIR}/tests/fixtures/sample_workpiece_grid.txt)
heartstead_add_smoke_test(smoke.world_inspector heartstead_world_inspector
                          ${PROJECT_SOURCE_DIR}/tests/fixtures/minimal_save_snapshot.txt
                          ${PROJECT_SOURCE_DIR})
heartstead_add_smoke_test(smoke.log_viewer heartstead_log_viewer --help)
heartstead_add_smoke_test(smoke.map_profile_inspector heartstead_map_profile_inspector --help)
