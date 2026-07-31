add_executable(heartstead_render_benchmark main.cpp)
target_link_libraries(heartstead_render_benchmark PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_render_benchmark
  PRIVATE
    HEARTSTEAD_SOURCE_ROOT="${PROJECT_SOURCE_DIR}"
    HEARTSTEAD_RENDER_BENCHMARK_ASSET_DIR="$<TARGET_FILE_DIR:heartstead_render_benchmark>/assets"
    HEARTSTEAD_RENDER_BENCHMARK_COOKED_ASSET_DIR="${CMAKE_BINARY_DIR}/apps/dev_game/assets/cooked"
)
heartstead_configure_target(heartstead_render_benchmark)
heartstead_attach_terrain_shader_assets(heartstead_render_benchmark)
