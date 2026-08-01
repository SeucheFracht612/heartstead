add_executable(heartstead_asset_cooker main.cpp)
target_link_libraries(heartstead_asset_cooker PRIVATE Heartstead::Engine)
target_compile_definitions(
  heartstead_asset_cooker
  PRIVATE HEARTSTEAD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_target(heartstead_asset_cooker)

if(TARGET heartstead_dev_game OR TARGET heartstead)
  file(
    GLOB_RECURSE heartstead_dev_game_visual_sources
    CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/mods/*/data/entity_visuals/*.toml"
    "${PROJECT_SOURCE_DIR}/mods/*/data/audio/*.toml"
    "${PROJECT_SOURCE_DIR}/mods/*/data/materials/*.toml"
    "${PROJECT_SOURCE_DIR}/mods/*/data/vegetation/*.toml"
    "${PROJECT_SOURCE_DIR}/mods/*/assets/*"
    "${PROJECT_SOURCE_DIR}/resource_packs/*/resource_pack.toml"
    "${PROJECT_SOURCE_DIR}/resource_packs/*/assets/*"
  )
endif()

function(heartstead_attach_cooked_assets target cooked_root asset_target)
  set(cooked_manifest "${cooked_root}/asset_manifest.txt")
  add_custom_command(
    OUTPUT "${cooked_manifest}"
    COMMAND
      "$<TARGET_FILE:heartstead_asset_cooker>" "${PROJECT_SOURCE_DIR}"
      "${cooked_manifest}" production --presentation-assets
    DEPENDS heartstead_asset_cooker ${heartstead_dev_game_visual_sources}
    COMMENT "Cooking presentation assets for ${target}"
    VERBATIM
  )
  add_custom_target(
    ${asset_target}
    DEPENDS "${cooked_manifest}"
  )
  add_dependencies(${target} ${asset_target})
endfunction()

if(TARGET heartstead)
  heartstead_attach_cooked_assets(
    heartstead "${CMAKE_BINARY_DIR}/apps/heartstead/assets/cooked"
    heartstead_production_cooked_assets)
endif()

if(TARGET heartstead_dev_game)
  heartstead_attach_cooked_assets(
    heartstead_dev_game "${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
    heartstead_dev_game_model_assets)
  if(TARGET heartstead_render_benchmark)
    add_dependencies(heartstead_render_benchmark heartstead_dev_game_model_assets)
  endif()
  if(TARGET heartstead_asset_lab)
    add_dependencies(heartstead_asset_lab heartstead_dev_game_model_assets)
  endif()
endif()
