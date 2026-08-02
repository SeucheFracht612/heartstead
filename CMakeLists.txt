cmake_minimum_required(VERSION 3.24)

project(heartstead_voxel_engine VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(HEARTSTEAD_BUILD_TESTS "Build the test executable" ON)
option(HEARTSTEAD_BUILD_BENCHMARKS "Build the benchmark executable" ON)
option(HEARTSTEAD_ENABLE_NATIVE_ARCH "Optimize for the build machine's CPU" OFF)
option(HEARTSTEAD_BUILD_GAME "Build the windowed voxel demo" ON)

add_library(heartstead_engine
    src/voxel/chunk.cpp
    src/voxel/greedy_mesher.cpp
    src/world/chunk_world.cpp
    src/world/world_generation.cpp
)
add_library(Heartstead::Engine ALIAS heartstead_engine)
target_link_libraries(heartstead_engine PUBLIC Threads::Threads)

add_library(heartstead_gameplay
    src/game/camera.cpp
    src/game/interaction.cpp
    src/game/player.cpp
    src/world/world_streamer.cpp
)
add_library(Heartstead::Gameplay ALIAS heartstead_gameplay)
target_link_libraries(heartstead_gameplay PUBLIC Heartstead::Engine)
target_compile_features(heartstead_gameplay PUBLIC cxx_std_23)
target_include_directories(heartstead_gameplay PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(heartstead_engine PUBLIC cxx_std_23)
target_include_directories(heartstead_engine PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(heartstead_engine PRIVATE /W4 /permissive- /EHsc)
    target_compile_options(heartstead_gameplay PRIVATE /W4 /permissive- /EHsc)
    if(HEARTSTEAD_ENABLE_NATIVE_ARCH)
        target_compile_options(heartstead_engine PRIVATE /arch:AVX2)
    endif()
else()
    target_compile_options(heartstead_engine PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    target_compile_options(heartstead_gameplay PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(HEARTSTEAD_ENABLE_NATIVE_ARCH)
        target_compile_options(heartstead_engine PRIVATE -march=native)
    endif()
endif()

if(HEARTSTEAD_BUILD_GAME)
    include(FetchContent)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)

    add_executable(heartstead
        src/main.cpp
        src/app/menu_controller.cpp
        src/app/video_settings_controller.cpp
        src/app/world_editor.cpp
        src/app/world_save_store.cpp
        src/platform/input.cpp
        src/platform/window.cpp
        src/render/opengl_renderer.cpp
        src/render/ui/menu_ui.cpp
        src/render/ui/pixel_ui.cpp
    )
    target_link_libraries(heartstead PRIVATE Heartstead::Gameplay glfw)
    target_compile_features(heartstead PRIVATE cxx_std_23)
    if(MSVC)
        target_compile_options(heartstead PRIVATE /W4 /permissive- /EHsc)
    else()
        target_compile_options(heartstead PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    endif()
endif()

include(CTest)
if(BUILD_TESTING AND HEARTSTEAD_BUILD_TESTS)
    add_executable(heartstead_tests tests/voxel_tests.cpp)
    target_link_libraries(heartstead_tests PRIVATE Heartstead::Engine)
    add_test(NAME heartstead_tests COMMAND heartstead_tests)
endif()

if(HEARTSTEAD_BUILD_BENCHMARKS)
    add_executable(heartstead_bench benchmarks/voxel_bench.cpp)
    target_link_libraries(heartstead_bench PRIVATE Heartstead::Engine)
endif()
