add_library(
  heartstead_asset_lab_runtime STATIC
  asset_lab_mode.cpp
  asset_lab_mode.hpp
)
target_link_libraries(heartstead_asset_lab_runtime PUBLIC Heartstead::GameRuntime)
heartstead_configure_target(heartstead_asset_lab_runtime)

add_executable(heartstead_asset_lab main.cpp)
target_link_libraries(heartstead_asset_lab PRIVATE heartstead_asset_lab_runtime)
target_compile_definitions(
  heartstead_asset_lab
  PRIVATE
    HEARTSTEAD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    HEARTSTEAD_ASSET_LAB_ASSET_DIR="$<TARGET_FILE_DIR:heartstead_asset_lab>/assets"
    HEARTSTEAD_ASSET_LAB_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
)
heartstead_configure_target(heartstead_asset_lab)
heartstead_attach_terrain_shader_assets(heartstead_asset_lab)
