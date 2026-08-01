add_executable(heartstead main.cpp)
target_link_libraries(heartstead PRIVATE Heartstead::GameRuntime)
target_compile_definitions(
  heartstead
  PRIVATE
    HEARTSTEAD_GAME_CONTENT_DIR="assets/content"
    HEARTSTEAD_GAME_ASSET_DIR="assets"
    HEARTSTEAD_GAME_COOKED_ASSET_DIR="assets/cooked"
)
heartstead_configure_target(heartstead)
heartstead_attach_terrain_shader_assets(heartstead)

add_custom_target(
  heartstead_content_assets ALL
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:heartstead>/assets/content"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/mods"
          "$<TARGET_FILE_DIR:heartstead>/assets/content/mods"
  COMMAND ${CMAKE_COMMAND} -E copy_directory "${PROJECT_SOURCE_DIR}/resource_packs"
          "$<TARGET_FILE_DIR:heartstead>/assets/content/resource_packs"
  DEPENDS ${PROJECT_SOURCE_DIR}/mods ${PROJECT_SOURCE_DIR}/resource_packs
  COMMENT "Staging production content beside heartstead"
  VERBATIM
)
add_dependencies(heartstead heartstead_content_assets)
