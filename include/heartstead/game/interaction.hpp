#pragma once

#include "heartstead/game/player.hpp"

#include <optional>

namespace heartstead::game {

struct BlockRayHit {
    Int3 block{};
    Int3 adjacent{};
    Float3 point{};
};

[[nodiscard]] std::optional<BlockRayHit> raycast_block(
    const ChunkWorld& world,
    Float3 origin,
    Float3 direction,
    float maximum_distance = 6.0F) noexcept;

[[nodiscard]] std::optional<BlockRayHit> interaction_raycast(
    const ChunkWorld& world,
    const Camera& camera,
    const Player& player,
    CameraMode mode) noexcept;

[[nodiscard]] bool block_intersects_player(Int3 block, const Player& player) noexcept;

} // namespace heartstead::game
