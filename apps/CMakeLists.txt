find_program(
  HEARTSTEAD_GLSLANG_VALIDATOR
  NAMES glslangValidator glslang
  HINTS
    "${VCPKG_INSTALLED_DIR}/${VCPKG_HOST_TRIPLET}/tools/glslang"
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/glslang"
)

function(heartstead_attach_terrain_shader_assets target)
  set(shader_source_dir "${PROJECT_SOURCE_DIR}/engine/renderer/shaders/builtin")
  set(ui_font_source "${PROJECT_SOURCE_DIR}/mods/base/assets/fonts/heartstead-ui.ttf")
  set(compiled_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_compiled_shaders")
  set(sky_vertex_source "${shader_source_dir}/sky.vert")
  set(sky_fragment_source "${shader_source_dir}/sky.frag")
  set(vertex_source "${shader_source_dir}/terrain.vert")
  set(far_vertex_source "${shader_source_dir}/far_terrain.vert")
  set(fragment_source "${shader_source_dir}/terrain.frag")
  set(low_fragment_source "${shader_source_dir}/terrain_low.frag")
  set(static_vertex_source "${shader_source_dir}/static_mesh.vert")
  set(static_fragment_source "${shader_source_dir}/static_mesh.frag")
  set(shadow_terrain_fragment_source "${shader_source_dir}/shadow_terrain.frag")
  set(shadow_static_fragment_source "${shader_source_dir}/shadow_static.frag")
  set(debug_vertex_source "${shader_source_dir}/debug_line.vert")
  set(debug_fragment_source "${shader_source_dir}/debug_line.frag")
  set(ui_vertex_source "${shader_source_dir}/ui.vert")
  set(ui_fragment_source "${shader_source_dir}/ui.frag")
  set(tone_map_vertex_source "${shader_source_dir}/tone_map.vert")
  set(tone_map_fragment_source "${shader_source_dir}/tone_map.frag")
  set(ssao_fragment_source "${shader_source_dir}/ssao.frag")
  set(ao_composite_fragment_source "${shader_source_dir}/ao_composite.frag")
  set(fxaa_fragment_source "${shader_source_dir}/fxaa.frag")
  set(bloom_fragment_source "${shader_source_dir}/bloom.frag")

  if(HEARTSTEAD_GLSLANG_VALIDATOR)
    set(sky_vertex_spirv "${compiled_shader_dir}/sky.vert.spv")
    set(sky_fragment_spirv "${compiled_shader_dir}/sky.frag.spv")
    set(vertex_spirv "${compiled_shader_dir}/terrain.vert.spv")
    set(far_vertex_spirv "${compiled_shader_dir}/far_terrain.vert.spv")
    set(fragment_spirv "${compiled_shader_dir}/terrain.frag.spv")
    set(low_fragment_spirv "${compiled_shader_dir}/terrain_low.frag.spv")
    set(static_vertex_spirv "${compiled_shader_dir}/static_mesh.vert.spv")
    set(static_fragment_spirv "${compiled_shader_dir}/static_mesh.frag.spv")
    set(shadow_terrain_fragment_spirv "${compiled_shader_dir}/shadow_terrain.frag.spv")
    set(shadow_static_fragment_spirv "${compiled_shader_dir}/shadow_static.frag.spv")
    set(debug_vertex_spirv "${compiled_shader_dir}/debug_line.vert.spv")
    set(debug_fragment_spirv "${compiled_shader_dir}/debug_line.frag.spv")
    set(ui_vertex_spirv "${compiled_shader_dir}/ui.vert.spv")
    set(ui_fragment_spirv "${compiled_shader_dir}/ui.frag.spv")
    set(tone_map_vertex_spirv "${compiled_shader_dir}/tone_map.vert.spv")
    set(tone_map_fragment_spirv "${compiled_shader_dir}/tone_map.frag.spv")
    set(ssao_fragment_spirv "${compiled_shader_dir}/ssao.frag.spv")
    set(ao_composite_fragment_spirv "${compiled_shader_dir}/ao_composite.frag.spv")
    set(fxaa_fragment_spirv "${compiled_shader_dir}/fxaa.frag.spv")
    set(bloom_fragment_spirv "${compiled_shader_dir}/bloom.frag.spv")
    add_custom_command(
      OUTPUT "${sky_vertex_spirv}" "${sky_fragment_spirv}"
             "${vertex_spirv}" "${far_vertex_spirv}" "${fragment_spirv}" "${low_fragment_spirv}"
             "${static_vertex_spirv}"
             "${static_fragment_spirv}" "${debug_vertex_spirv}" "${debug_fragment_spirv}"
             "${shadow_terrain_fragment_spirv}" "${shadow_static_fragment_spirv}"
             "${ui_vertex_spirv}" "${ui_fragment_spirv}"
             "${tone_map_vertex_spirv}" "${tone_map_fragment_spirv}"
             "${ssao_fragment_spirv}" "${ao_composite_fragment_spirv}"
             "${fxaa_fragment_spirv}" "${bloom_fragment_spirv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${compiled_shader_dir}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${sky_vertex_source}" -o "${sky_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${sky_fragment_source}" -o "${sky_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${vertex_source}" -o "${vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${far_vertex_source}" -o "${far_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${fragment_source}" -o "${fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${low_fragment_source}" -o "${low_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${static_vertex_source}" -o "${static_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${static_fragment_source}" -o "${static_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${shadow_terrain_fragment_source}" -o "${shadow_terrain_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${shadow_static_fragment_source}" -o "${shadow_static_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${debug_vertex_source}" -o "${debug_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${debug_fragment_source}" -o "${debug_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${ui_vertex_source}" -o "${ui_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${ui_fragment_source}" -o "${ui_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${tone_map_vertex_source}" -o "${tone_map_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${tone_map_fragment_source}" -o "${tone_map_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${ssao_fragment_source}" -o "${ssao_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${ao_composite_fragment_source}" -o "${ao_composite_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${fxaa_fragment_source}" -o "${fxaa_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${bloom_fragment_source}" -o "${bloom_fragment_spirv}"
      DEPENDS "${sky_vertex_source}" "${sky_fragment_source}"
              "${vertex_source}" "${far_vertex_source}" "${fragment_source}"
              "${low_fragment_source}" "${static_vertex_source}"
              "${static_fragment_source}" "${shadow_terrain_fragment_source}"
              "${shadow_static_fragment_source}" "${debug_vertex_source}" "${debug_fragment_source}"
              "${ui_vertex_source}" "${ui_fragment_source}"
              "${tone_map_vertex_source}" "${tone_map_fragment_source}"
              "${ssao_fragment_source}" "${ao_composite_fragment_source}"
              "${fxaa_fragment_source}" "${bloom_fragment_source}"
      COMMENT "Compiling renderer GLSL to SPIR-V"
      VERBATIM
    )
  else()
    set(sky_vertex_spirv "${shader_source_dir}/sky.vert.spv")
    set(sky_fragment_spirv "${shader_source_dir}/sky.frag.spv")
    set(vertex_spirv "${shader_source_dir}/terrain.vert.spv")
    set(far_vertex_spirv "${shader_source_dir}/far_terrain.vert.spv")
    set(fragment_spirv "${shader_source_dir}/terrain.frag.spv")
    set(low_fragment_spirv "${shader_source_dir}/terrain_low.frag.spv")
    set(static_vertex_spirv "${shader_source_dir}/static_mesh.vert.spv")
    set(static_fragment_spirv "${shader_source_dir}/static_mesh.frag.spv")
    set(shadow_terrain_fragment_spirv "${shader_source_dir}/shadow_terrain.frag.spv")
    set(shadow_static_fragment_spirv "${shader_source_dir}/shadow_static.frag.spv")
    set(debug_vertex_spirv "${shader_source_dir}/debug_line.vert.spv")
    set(debug_fragment_spirv "${shader_source_dir}/debug_line.frag.spv")
    set(ui_vertex_spirv "${shader_source_dir}/ui.vert.spv")
    set(ui_fragment_spirv "${shader_source_dir}/ui.frag.spv")
    set(tone_map_vertex_spirv "${shader_source_dir}/tone_map.vert.spv")
    set(tone_map_fragment_spirv "${shader_source_dir}/tone_map.frag.spv")
    set(ssao_fragment_spirv "${shader_source_dir}/ssao.frag.spv")
    set(ao_composite_fragment_spirv "${shader_source_dir}/ao_composite.frag.spv")
    set(fxaa_fragment_spirv "${shader_source_dir}/fxaa.frag.spv")
    set(bloom_fragment_spirv "${shader_source_dir}/bloom.frag.spv")
  endif()

  add_custom_target(
    ${target}_shader_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets/shaders"
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets/fonts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_font_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/fonts/heartstead-ui.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sky_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/sky.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sky_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/sky.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${far_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/far_terrain.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${low_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain_low.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shadow_terrain_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/shadow_terrain.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shadow_static_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/shadow_static.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${debug_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/debug_line.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${debug_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/debug_line.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ui.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ui.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sky_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/sky.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sky_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/sky.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${far_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/far_terrain.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${low_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain_low.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shadow_terrain_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/shadow_terrain.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${shadow_static_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/shadow_static.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${debug_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/debug_line.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${debug_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/debug_line.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ui.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ui.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tone_map_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/tone_map.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tone_map_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/tone_map.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ssao_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ssao.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ao_composite_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ao_composite.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fxaa_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/fxaa.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/bloom.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tone_map_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/tone_map.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${tone_map_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/tone_map.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ssao_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ssao.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ao_composite_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ao_composite.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fxaa_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/fxaa.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${bloom_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/bloom.frag.spv"
    DEPENDS "${sky_vertex_spirv}" "${sky_fragment_spirv}"
            "${vertex_spirv}" "${far_vertex_spirv}" "${fragment_spirv}" "${low_fragment_spirv}"
            "${static_vertex_spirv}"
            "${static_fragment_spirv}" "${shadow_terrain_fragment_spirv}"
            "${shadow_static_fragment_spirv}" "${debug_vertex_spirv}" "${debug_fragment_spirv}"
            "${ui_vertex_spirv}" "${ui_fragment_spirv}"
            "${tone_map_vertex_spirv}" "${tone_map_fragment_spirv}"
            "${ssao_fragment_spirv}" "${ao_composite_fragment_spirv}"
            "${fxaa_fragment_spirv}" "${bloom_fragment_spirv}" "${ui_font_source}"
    COMMENT "Staging compiled renderer shader assets for ${target}"
    VERBATIM
  )
  add_dependencies(${target} ${target}_shader_assets)
endfunction()

add_subdirectory(render_smoke)
add_subdirectory(render_benchmark)
add_subdirectory(voxel_benchmark)
add_subdirectory(voxel_meshing_benchmark)
add_subdirectory(chunk_streaming_benchmark)
add_subdirectory(predictive_streaming_benchmark)
add_subdirectory(process_temporal_aggregation_benchmark)
add_subdirectory(multiplayer_chunk_subscription_benchmark)
add_subdirectory(multiplayer_network_impairment_benchmark)
add_subdirectory(chunk_delta_journal_benchmark)
add_subdirectory(chunk_render_readiness_benchmark)
add_subdirectory(terrain_edit_transition_benchmark)
add_subdirectory(voxel_response_benchmark)
add_subdirectory(audio_benchmark)
add_subdirectory(ui_benchmark)
add_subdirectory(scripting_benchmark)
add_subdirectory(heartstead)
add_subdirectory(dev_game)
add_subdirectory(asset_lab)
add_subdirectory(dedicated_server)
