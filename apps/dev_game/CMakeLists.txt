add_executable(
  heartstead_dev_game
  main.cpp
  dev_game_mode.cpp
  dev_game_mode.hpp
)
target_link_libraries(heartstead_dev_game PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_dev_game
  PRIVATE
    HEARTSTEAD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    HEARTSTEAD_DEV_GAME_ASSET_DIR="$<TARGET_FILE_DIR:heartstead_dev_game>/assets"
    HEARTSTEAD_DEV_GAME_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
)
heartstead_configure_target(heartstead_dev_game)
heartstead_attach_terrain_shader_assets(heartstead_dev_game)
