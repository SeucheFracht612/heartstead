add_executable(heartstead_asset_cooker main.cpp)
target_link_libraries(heartstead_asset_cooker PRIVATE Heartstead::Engine)
target_compile_definitions(
  heartstead_asset_cooker
  PRIVATE HEARTSTEAD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_target(heartstead_asset_cooker)

if(TARGET heartstead_dev_game)
  set(heartstead_dev_game_model
      "${PROJECT_SOURCE_DIR}/mods/base/assets/models/entities/storybook_player.gltf")
  set(heartstead_dev_game_animal_model
      "${PROJECT_SOURCE_DIR}/mods/base/assets/models/entities/storybook_animal.gltf")
  set(heartstead_dev_game_cooked_root
      "${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked")
  set(heartstead_dev_game_cooked_manifest
      "${heartstead_dev_game_cooked_root}/asset_manifest.txt")
  add_custom_command(
    OUTPUT "${heartstead_dev_game_cooked_manifest}"
    COMMAND
      "$<TARGET_FILE:heartstead_asset_cooker>" "${PROJECT_SOURCE_DIR}"
      "${heartstead_dev_game_cooked_manifest}" production --only
      base:models/entities/storybook_player.gltf
      --only base:models/entities/storybook_animal.gltf
    DEPENDS heartstead_asset_cooker "${heartstead_dev_game_model}"
            "${heartstead_dev_game_animal_model}"
    COMMENT "Cooking the dev-game model set"
    VERBATIM
  )
  add_custom_target(
    heartstead_dev_game_model_assets
    DEPENDS "${heartstead_dev_game_cooked_manifest}"
  )
  add_dependencies(heartstead_dev_game heartstead_dev_game_model_assets)
endif()
