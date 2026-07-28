add_library(heartstead_game)
add_library(Heartstead::GameRuntime ALIAS heartstead_game)

target_sources(
  heartstead_game
  PRIVATE
    features/interaction/voxel_raycast.cpp
    features/interaction/voxel_commands.cpp
    features/interaction/voxel_interaction_module.cpp
    features/animals/wandering_animal_module.cpp
    framework/gameplay_module.cpp
    framework/domain_service_registry.cpp
    presentation/animated_model_presentation.cpp
    presentation/client_audio_presentation.cpp
    presentation/client_presentation.cpp
    presentation/particle_presentation.cpp
    presentation/presentation_world.cpp
    runtime/client_runtime.cpp
    runtime/game_inspection.cpp
    runtime/game_runtime.cpp
    runtime/runtime_session.cpp
    runtime/server_runtime.cpp
    runtime/script_host_commands.cpp
    testing/headless_session.cpp
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}
    FILES
      features/interaction/voxel_raycast.hpp
      features/interaction/voxel_commands.hpp
      features/interaction/voxel_interaction_module.hpp
      features/animals/wandering_animal_module.hpp
      framework/gameplay_module.hpp
      framework/domain_service_registry.hpp
      presentation/animated_model_presentation.hpp
      presentation/client_audio_presentation.hpp
      presentation/client_presentation.hpp
      presentation/particle_presentation.hpp
      presentation/presentation_world.hpp
      presentation/render_snapshot.hpp
      runtime/client_runtime.hpp
      runtime/game_inspection.hpp
      runtime/game_runtime.hpp
      runtime/runtime_session.hpp
      runtime/server_runtime.hpp
      runtime/script_host_commands.hpp
      testing/headless_session.hpp
)

target_link_libraries(heartstead_game PUBLIC Heartstead::Engine)
target_include_directories(heartstead_game PUBLIC ${PROJECT_SOURCE_DIR})

heartstead_configure_target(heartstead_game)
