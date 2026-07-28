find_program(HEARTSTEAD_GLSLANG_VALIDATOR NAMES glslangValidator glslang)

function(heartstead_attach_terrain_shader_assets target)
  set(shader_source_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/render_smoke/assets/shaders")
  set(compiled_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/${target}_compiled_shaders")
  set(sky_vertex_source "${shader_source_dir}/sky.vert")
  set(sky_fragment_source "${shader_source_dir}/sky.frag")
  set(vertex_source "${shader_source_dir}/terrain.vert")
  set(fragment_source "${shader_source_dir}/terrain.frag")
  set(static_vertex_source "${shader_source_dir}/static_mesh.vert")
  set(static_fragment_source "${shader_source_dir}/static_mesh.frag")
  set(debug_vertex_source "${shader_source_dir}/debug_line.vert")
  set(debug_fragment_source "${shader_source_dir}/debug_line.frag")
  set(ui_vertex_source "${shader_source_dir}/ui.vert")
  set(ui_fragment_source "${shader_source_dir}/ui.frag")

  if(HEARTSTEAD_GLSLANG_VALIDATOR)
    set(sky_vertex_spirv "${compiled_shader_dir}/sky.vert.spv")
    set(sky_fragment_spirv "${compiled_shader_dir}/sky.frag.spv")
    set(vertex_spirv "${compiled_shader_dir}/terrain.vert.spv")
    set(fragment_spirv "${compiled_shader_dir}/terrain.frag.spv")
    set(static_vertex_spirv "${compiled_shader_dir}/static_mesh.vert.spv")
    set(static_fragment_spirv "${compiled_shader_dir}/static_mesh.frag.spv")
    set(debug_vertex_spirv "${compiled_shader_dir}/debug_line.vert.spv")
    set(debug_fragment_spirv "${compiled_shader_dir}/debug_line.frag.spv")
    set(ui_vertex_spirv "${compiled_shader_dir}/ui.vert.spv")
    set(ui_fragment_spirv "${compiled_shader_dir}/ui.frag.spv")
    add_custom_command(
      OUTPUT "${sky_vertex_spirv}" "${sky_fragment_spirv}"
             "${vertex_spirv}" "${fragment_spirv}" "${static_vertex_spirv}"
             "${static_fragment_spirv}" "${debug_vertex_spirv}" "${debug_fragment_spirv}"
             "${ui_vertex_spirv}" "${ui_fragment_spirv}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${compiled_shader_dir}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${sky_vertex_source}" -o "${sky_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${sky_fragment_source}" -o "${sky_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${vertex_source}" -o "${vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${fragment_source}" -o "${fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${static_vertex_source}" -o "${static_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${static_fragment_source}" -o "${static_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${debug_vertex_source}" -o "${debug_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${debug_fragment_source}" -o "${debug_fragment_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${ui_vertex_source}" -o "${ui_vertex_spirv}"
      COMMAND "${HEARTSTEAD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0
              "${ui_fragment_source}" -o "${ui_fragment_spirv}"
      DEPENDS "${sky_vertex_source}" "${sky_fragment_source}"
              "${vertex_source}" "${fragment_source}" "${static_vertex_source}"
              "${static_fragment_source}" "${debug_vertex_source}" "${debug_fragment_source}"
              "${ui_vertex_source}" "${ui_fragment_source}"
      COMMENT "Compiling renderer GLSL to SPIR-V"
      VERBATIM
    )
  else()
    set(sky_vertex_spirv "${shader_source_dir}/sky.vert.spv")
    set(sky_fragment_spirv "${shader_source_dir}/sky.frag.spv")
    set(vertex_spirv "${shader_source_dir}/terrain.vert.spv")
    set(fragment_spirv "${shader_source_dir}/terrain.frag.spv")
    set(static_vertex_spirv "${shader_source_dir}/static_mesh.vert.spv")
    set(static_fragment_spirv "${shader_source_dir}/static_mesh.frag.spv")
    set(debug_vertex_spirv "${shader_source_dir}/debug_line.vert.spv")
    set(debug_fragment_spirv "${shader_source_dir}/debug_line.frag.spv")
    set(ui_vertex_spirv "${shader_source_dir}/ui.vert.spv")
    set(ui_fragment_spirv "${shader_source_dir}/ui.frag.spv")
  endif()

  add_custom_target(
    ${target}_shader_assets ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/assets/shaders"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sky_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/sky.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${sky_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/sky.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.frag"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_vertex_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.vert"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_fragment_source}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.frag"
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
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/terrain.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${static_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/static_mesh.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${debug_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/debug_line.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${debug_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/debug_line.frag.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_vertex_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ui.vert.spv"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${ui_fragment_spirv}"
            "$<TARGET_FILE_DIR:${target}>/assets/shaders/ui.frag.spv"
    DEPENDS "${sky_vertex_spirv}" "${sky_fragment_spirv}"
            "${vertex_spirv}" "${fragment_spirv}" "${static_vertex_spirv}"
            "${static_fragment_spirv}" "${debug_vertex_spirv}" "${debug_fragment_spirv}"
            "${ui_vertex_spirv}" "${ui_fragment_spirv}"
    COMMENT "Staging compiled renderer shader assets for ${target}"
    VERBATIM
  )
  add_dependencies(${target} ${target}_shader_assets)
endfunction()

add_subdirectory(render_smoke)
add_subdirectory(render_benchmark)
add_subdirectory(audio_benchmark)
add_subdirectory(ui_benchmark)
add_subdirectory(scripting_benchmark)
add_subdirectory(dev_game)
add_subdirectory(dedicated_server)
