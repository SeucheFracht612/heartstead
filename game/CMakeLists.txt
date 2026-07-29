add_library(heartstead_game)
add_library(Heartstead::GameRuntime ALIAS heartstead_game)

target_sources(
  heartstead_game
  PRIVATE
    application/game_application.cpp
    foundation/foundation_world.cpp
    features/interaction/voxel_raycast.cpp
    features/interaction/voxel_commands.cpp
    features/interaction/voxel_interaction_module.cpp
    features/animals/wandering_animal_module.cpp
    framework/gameplay_module.cpp
    framework/domain_service_registry.cpp
    presentation/animated_model_presentation.cpp
    presentation/client_audio_presentation.cpp
    presentation/client_presentation.cpp
    presentation/model_presentation_system.cpp
    presentation/particle_presentation.cpp
    presentation/presentation_world.cpp
    presentation/voxel_interaction_presentation.cpp
    runtime/client_runtime.cpp
    runtime/game_inspection.cpp
    runtime/game_runtime.cpp
    runtime/runtime_session.cpp
    runtime/server_runtime.cpp
    runtime/script_host_commands.cpp
    testing/headless_session.cpp
    ui/game_ui.cpp
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS ${PROJECT_SOURCE_DIR}
    FILES
      application/game_application.hpp
      foundation/foundation_world.hpp
      features/interaction/voxel_raycast.hpp
      features/interaction/voxel_commands.hpp
      features/interaction/voxel_interaction_module.hpp
      features/animals/wandering_animal_module.hpp
      framework/gameplay_module.hpp
      framework/domain_service_registry.hpp
      presentation/animated_model_presentation.hpp
      presentation/client_audio_presentation.hpp
      presentation/client_presentation.hpp
      presentation/model_presentation_system.hpp
      presentation/particle_presentation.hpp
      presentation/presentation_world.hpp
      presentation/render_snapshot.hpp
      presentation/voxel_interaction_presentation.hpp
      runtime/client_runtime.hpp
      runtime/game_inspection.hpp
      runtime/game_runtime.hpp
      runtime/runtime_session.hpp
      runtime/server_runtime.hpp
      runtime/script_host_commands.hpp
      testing/headless_session.hpp
      ui/game_ui.hpp
)

target_link_libraries(heartstead_game PUBLIC Heartstead::Engine)
target_include_directories(heartstead_game PUBLIC ${PROJECT_SOURCE_DIR})

heartstead_configure_target(heartstead_game)
