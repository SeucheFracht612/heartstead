add_executable(heartstead_multiplayer_network_impairment_benchmark main.cpp)
target_link_libraries(heartstead_multiplayer_network_impairment_benchmark
                      PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead_multiplayer_network_impairment_benchmark
  PRIVATE HEARTSTEAD_BENCHMARK_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)
heartstead_configure_target(heartstead_multiplayer_network_impairment_benchmark)
