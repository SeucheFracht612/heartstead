#pragma once

#include "heartstead/game/camera.hpp"
#include "heartstead/world/chunk_world.hpp"

namespace heartstead::game {

enum class MovementMode { walking, flying };

struct Player {
    static constexpr float radius = 0.34F;
    static constexpr float height = 1.95F;
    static constexpr float eye_height = 1.74F;

    Float3 position{0.5F, 40.0F, 0.5F};
    Float3 velocity{};
    float yaw{-1.57079633F};
    bool grounded{};
    MovementMode movement_mode{MovementMode::walking};
};

struct PlayerInput {
    float forward{};
    float right{};
    bool jump{};
    bool descend{};
    bool sprint{};
};

[[nodiscard]] bool player_collides(const ChunkWorld& world, Float3 position) noexcept;
[[nodiscard]] float surface_height(const ChunkWorld& world, float x, float z) noexcept;
void toggle_flight(Player& player) noexcept;

void update_player(
    Player& player,
    const Camera& camera,
    const ChunkWorld& world,
    const PlayerInput& input,
    float delta_seconds,
    CameraMode mode);

void update_camera_pose(
    Camera& camera,
    const Player& player,
    const ChunkWorld& world,
    CameraMode mode);

} // namespace heartstead::game
