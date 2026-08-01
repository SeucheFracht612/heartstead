add_executable(heartstead main.cpp)
target_link_libraries(heartstead PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead
  PRIVATE
    HEARTSTEAD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    HEARTSTEAD_GAME_ASSET_DIR="$<TARGET_FILE_DIR:heartstead>/assets"
    HEARTSTEAD_GAME_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
)
heartstead_configure_target(heartstead)
heartstead_attach_terrain_shader_assets(heartstead)
